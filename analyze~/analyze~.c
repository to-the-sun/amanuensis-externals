#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "ext_path.h"
#include "z_dsp.h"
#include "cumulative_transience.h"
#include "../shared/async_worker.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define MAX_AUDIO_SECONDS 60
#define ANALYSIS_HOP_MS 100

typedef struct _shared_buffer_entry {
    t_symbol* name;
    SharedTransientBuffer* buffer;
    t_critical lock;
    int ref_count;
    struct _shared_buffer_entry* next;
} t_shared_buffer_entry;

static t_shared_buffer_entry* g_shared_buffers = NULL;
static t_critical g_shared_buffers_lock;

typedef struct _analyze {
    t_pxobject obj;

    // Outlets
    void* outlet_list;      // Band, Score
    void* outlet_barlen;
    void* outlet_rating;
    void* outlet_stddev;
    void* outlet_contrast;
    void* outlet_peakstd;
    void* outlet_log;

    // Attributes
    long log_enabled;
    t_symbol* group_name;

    // Analyzer State
    TransientAnalyzer* analyzer;
    double sample_rate;

    // Circular Buffer for Audio and Clock
    float* audio_buffer;
    double* clock_buffer;
    int audio_buffer_size;
    int audio_buffer_write_ptr;

    // Processing State
    volatile long long current_sample_count;
    volatile long long last_analysis_frame;
    int last_peak_frame[MAX_BANDS];
    long clock_connected;

    // Async Worker
    t_async_worker* worker;
    t_critical lock;
    int invalidated;
    int pending_analysis;

    ChunkAnalysisResult* result_buffer;

} t_analyze;

void* analyze_new(t_symbol* s, long argc, t_atom* argv);
void analyze_free(t_analyze* x);
void analyze_clear(t_analyze* x);
void analyze_group_settor(t_analyze* x, void* attr, long argc, t_atom* argv);
void analyze_worker_task(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_metrics(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_peak(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_log(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_log(t_analyze* x, const char* fmt, ...);
void analyze_dsp64(t_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void analyze_perform64(t_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam);
void analyze_assist(t_analyze* x, void* b, long m, long a, char* s);

static t_class* analyze_class;

void ext_main(void* r) {
    t_class* c = class_new("analyze~", (method)analyze_new, (method)analyze_free, sizeof(t_analyze), 0L, A_GIMME, 0);

    critical_new(&g_shared_buffers_lock);

    CLASS_ATTR_LONG(c, "log", 0, t_analyze, log_enabled);
    CLASS_ATTR_FILTER_CLIP(c, "log", 0, 1);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "checkbox", "Log Diagnostics");

    CLASS_ATTR_SYM(c, "group", 0, t_analyze, group_name);
    CLASS_ATTR_ACCESSORS(c, "group", NULL, (method)analyze_group_settor);
    CLASS_ATTR_LABEL(c, "group", 0, "Shared Group Name");

    class_addmethod(c, (method)analyze_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)analyze_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)analyze_clear, "clear", 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    analyze_class = c;
}

void* analyze_new(t_symbol* s, long argc, t_atom* argv) {
    t_analyze* x = (t_analyze*)object_alloc(analyze_class);

    if (x) {
        dsp_setup((t_pxobject*)x, 2);

        x->outlet_log = outlet_new(x, NULL);        // Outlet 6
        x->outlet_peakstd = floatout(x);           // Outlet 5 (Stability)
        x->outlet_contrast = floatout(x);          // Outlet 4
        x->outlet_stddev = floatout(x);            // Outlet 3
        x->outlet_rating = floatout(x);            // Outlet 2
        x->outlet_barlen = floatout(x);            // Outlet 1
        x->outlet_list = listout(x);               // Outlet 0

        critical_new(&x->lock);

        x->group_name = gensym("");
        x->analyzer = NULL;
        x->worker = async_worker_create();

        x->audio_buffer = NULL;
        x->clock_buffer = NULL;
        x->audio_buffer_size = 0;
        x->audio_buffer_write_ptr = 0;
        x->current_sample_count = 0;
        x->last_analysis_frame = 0;
        for(int i=0; i<MAX_BANDS; i++) x->last_peak_frame[i] = -1;

        x->invalidated = 0;
        x->pending_analysis = 0;
        x->log_enabled = 0;
        x->sample_rate = 44100.0;
        x->result_buffer = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));

        attr_args_process(x, argc, argv);

        if (!x->analyzer) {
            x->analyzer = analyzer_create(1.0, NULL, x->lock, (ct_lock_func)critical_enter, (ct_lock_func)critical_exit);
        }
    }
    return x;
}

void analyze_free(t_analyze* x) {
    dsp_free((t_pxobject*)x);

    critical_enter(x->lock);
    x->invalidated = 1;
    critical_exit(x->lock);

    if (x->worker) {
        async_worker_release(x->worker);
    }

    if (x->analyzer) {
        analyzer_destroy(x->analyzer);
    }

    if (x->group_name && x->group_name != gensym("")) {
        critical_enter(g_shared_buffers_lock);
        t_shared_buffer_entry* curr = g_shared_buffers;
        t_shared_buffer_entry* prev = NULL;
        while (curr) {
            if (curr->name == x->group_name) {
                curr->ref_count--;
                if (curr->ref_count <= 0) {
                    if (prev) prev->next = curr->next;
                    else g_shared_buffers = curr->next;
                    critical_free(curr->lock);
                    free(curr->buffer);
                    free(curr);
                }
                break;
            }
            prev = curr;
            curr = curr->next;
        }
        critical_exit(g_shared_buffers_lock);
    }

    if (x->result_buffer) free(x->result_buffer);

    free(x->audio_buffer);
    free(x->clock_buffer);
    critical_free(x->lock);
}

void analyze_clear(t_analyze* x) {
    critical_enter(x->lock);
    if (x->analyzer) {
        analyzer_clear(x->analyzer);
    }
    x->audio_buffer_write_ptr = 0;
    x->current_sample_count = 0;
    x->last_analysis_frame = 0;
    for (int i = 0; i < MAX_BANDS; i++) x->last_peak_frame[i] = -1;

    if (x->audio_buffer && x->audio_buffer_size > 0) {
        memset(x->audio_buffer, 0, sizeof(float) * x->audio_buffer_size);
    }
    if (x->clock_buffer && x->audio_buffer_size > 0) {
        memset(x->clock_buffer, 0, sizeof(double) * x->audio_buffer_size);
    }
    critical_exit(x->lock);
    analyze_log(x, "cleared internal state");
}

void analyze_group_settor(t_analyze* x, void* attr, long argc, t_atom* argv) {
    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        t_symbol* name = atom_getsym(argv);
        if (name != x->group_name) {
            if (x->analyzer) {
                object_error((t_object*)x, "cannot change @group after initialization");
                return;
            }
            x->group_name = name;
            if (x->group_name != gensym("")) {
                critical_enter(g_shared_buffers_lock);
                t_shared_buffer_entry* curr = g_shared_buffers;
                while (curr) {
                    if (curr->name == x->group_name) {
                        curr->ref_count++;
                        x->analyzer = analyzer_create(1.0, curr->buffer, curr->lock, (ct_lock_func)critical_enter, (ct_lock_func)critical_exit);
                        break;
                    }
                    curr = curr->next;
                }
                if (!curr) {
                    t_shared_buffer_entry* entry = (t_shared_buffer_entry*)malloc(sizeof(t_shared_buffer_entry));
                    entry->name = x->group_name;
                    entry->buffer = (SharedTransientBuffer*)calloc(1, sizeof(SharedTransientBuffer));
                    entry->buffer->min_score_seen = DBL_MAX;
                    entry->buffer->max_score_seen = -DBL_MAX;
                    entry->buffer->max_peak = 1.0;
                    critical_new(&entry->lock);
                    entry->ref_count = 1;
                    entry->next = g_shared_buffers;
                    g_shared_buffers = entry;
                    x->analyzer = analyzer_create(1.0, entry->buffer, entry->lock, (ct_lock_func)critical_enter, (ct_lock_func)critical_exit);
                }
                critical_exit(g_shared_buffers_lock);
            }
        }
    }
}

void analyze_assist(t_analyze* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(signal) Audio Input"); break;
            case 1: sprintf(s, "(signal) Transport Clock Input"); break;
        }
    } else {
        switch (a) {
            case 0: sprintf(s, "(list) Clock, Band, Score"); break;
            case 1: sprintf(s, "(float) Bar Length (ms)"); break;
            case 2: sprintf(s, "(float) Rating Score"); break;
            case 3: sprintf(s, "(float) Standard Deviation"); break;
            case 4: sprintf(s, "(float) Contrast Score"); break;
            case 5: sprintf(s, "(float) Bar Length Stability"); break;
            case 6: sprintf(s, "(symbol) Log Diagnostics"); break;
        }
    }
}

void analyze_log(t_analyze* x, const char* fmt, ...) {
    if (x->log_enabled && x->outlet_log) {
        char buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 4096, fmt, args);
        va_end(args);

        t_atom a;
        atom_setsym(&a, gensym(buf));
        defer(x, (method)analyze_output_log, NULL, 1, &a);
    }
}

void analyze_output_log(t_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    if (!x->invalidated && x->outlet_log) {
        outlet_anything(x->outlet_log, atom_getsym(argv), 0, NULL);
    }
}

void analyze_dsp64(t_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    x->sample_rate = samplerate;
    x->clock_connected = count[1];
    analyzer_set_sample_rate(x->analyzer, (int)samplerate);

    int new_size = (int)(samplerate * MAX_AUDIO_SECONDS);
    if (x->audio_buffer_size != new_size) {
        analyze_log(x, "reallocating audio and clock buffers: %d samples (%.1f seconds at %.1f Hz)", new_size, MAX_AUDIO_SECONDS, samplerate);
        free(x->audio_buffer);
        free(x->clock_buffer);
        x->audio_buffer = (float*)calloc(new_size, sizeof(float));
        x->clock_buffer = (double*)calloc(new_size, sizeof(double));
        x->audio_buffer_size = new_size;
        x->audio_buffer_write_ptr = 0;
        x->current_sample_count = 0;
        x->last_analysis_frame = 0;
    }

    dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)analyze_perform64, 0, NULL);
}

void analyze_perform64(t_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam) {
    double* in = ins[0];
    double* clock_in = ins[1];

    for (int i = 0; i < sampleframes; i++) {
        x->audio_buffer[x->audio_buffer_write_ptr] = (float)in[i];
        x->clock_buffer[x->audio_buffer_write_ptr] = clock_in[i];
        x->audio_buffer_write_ptr = (x->audio_buffer_write_ptr + 1) % x->audio_buffer_size;
        x->current_sample_count++;
    }

    int hop_samples = (int)(x->sample_rate * 0.001 * ANALYSIS_HOP_MS);
    if (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
        if (!x->pending_analysis) {
            x->pending_analysis = 1;
            analyze_log(x, "triggering worker task, current_sample_count: %lld, last_analysis_frame: %lld", x->current_sample_count, x->last_analysis_frame);
            async_worker_enqueue(x->worker, x, (method)analyze_worker_task, gensym("analyze"), 0, NULL);
        }
    }
}

void analyze_worker_task(t_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    if (x->invalidated || !x->analyzer) {
        x->pending_analysis = 0;
        return;
    }

    int hop_samples = (int)(x->sample_rate * 0.1);
    int ms_samples = (int)(x->sample_rate * 0.001);

    int hops_processed = 0;
    // We process all hops that have accumulated since last_analysis_frame
    while (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
        // Capture current state for consistent indexing within this hop
        long long cur_samples = x->current_sample_count;
        int cur_write_ptr = x->audio_buffer_write_ptr;

        if (x->invalidated) break;

        long long target_analysis_frame = x->last_analysis_frame + hop_samples;
        int hop_start_samples = (int)(target_analysis_frame - hop_samples);

        float* hop_audio = (float*)malloc(sizeof(float) * hop_samples);
        if (!hop_audio) break;

        // Calculate read pointer relative to current write pointer
        // We need the data that was at [hop_start_samples, target_analysis_frame]
        long long samples_ago = x->current_sample_count - hop_start_samples;
        int read_ptr = (int)((x->audio_buffer_write_ptr - samples_ago + x->audio_buffer_size) % x->audio_buffer_size);

        for (int i = 0; i < hop_samples; i++) {
            hop_audio[i] = x->audio_buffer[read_ptr];
            read_ptr = (read_ptr + 1) % x->audio_buffer_size;
        }

        int active_start_samples = (int)(target_analysis_frame - hop_samples - (int)(x->sample_rate * 0.2));
        int active_start_frame = active_start_samples / ms_samples;

        int window_start_samples = active_start_samples - (int)(x->sample_rate * 15.0);
        if (window_start_samples < 0) window_start_samples = 0;
        int buffer_start_frame = window_start_samples / ms_samples;

        if (x->result_buffer && analyzer_analyze_chunk(x->analyzer, hop_audio, hop_samples, (int)x->sample_rate, buffer_start_frame, active_start_frame, x->result_buffer)) {
            hops_processed++;
            for (int i = 0; i < x->result_buffer->peak_list.num_peaks; i++) {
                PeakResult* pr = &x->result_buffer->peak_list.peaks[i];

                if (x->clock_connected) {
                    long long peak_sample = (long long)pr->p_idx * ms_samples;
                    long long samples_ago = cur_samples - peak_sample;
                    int clock_idx = (int)((cur_write_ptr - samples_ago + x->audio_buffer_size) % x->audio_buffer_size);
                    double clock_val = x->clock_buffer[clock_idx];

                    t_atom out_args[3];
                    atom_setfloat(out_args, clock_val);
                    atom_setlong(out_args + 1, pr->band_idx);
                    atom_setfloat(out_args + 2, pr->total_score);
                    defer(x, (method)analyze_output_peak, NULL, 3, out_args);
                } else {
                    t_atom out_args[2];
                    atom_setlong(out_args, pr->band_idx);
                    atom_setfloat(out_args + 1, pr->total_score);
                    defer(x, (method)analyze_output_peak, NULL, 2, out_args);
                }
            }

            t_atom out_args[5];
            atom_setfloat(out_args, x->result_buffer->metrics.rating);
            atom_setfloat(out_args + 1, x->result_buffer->metrics.std_dev);
            atom_setfloat(out_args + 2, x->result_buffer->metrics.contrast);
            atom_setfloat(out_args + 3, x->result_buffer->metrics.stability_score);
            float barlen = x->result_buffer->metrics.highest_peak_valid ? (float)fabs(x->result_buffer->metrics.highest_peak_ms) : 0.0f;
            atom_setfloat(out_args + 4, barlen);
            defer(x, (method)analyze_output_metrics, NULL, 5, out_args);
        }

        free(hop_audio);
        x->last_analysis_frame = target_analysis_frame;
        analyze_log(x, "processed chunk at %lld samples, num_peaks: %d", target_analysis_frame, x->result_buffer->peak_list.num_peaks);
    }

    if (hops_processed > 1) {
        analyze_log(x, "catch-up complete: processed %d hops", hops_processed);
    }

    x->pending_analysis = 0;
}

void analyze_output_peak(t_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    outlet_list(x->outlet_list, NULL, argc, argv);
}

void analyze_output_metrics(t_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    outlet_float(x->outlet_peakstd, atom_getfloat(argv + 3));
    outlet_float(x->outlet_contrast, atom_getfloat(argv + 2));
    outlet_float(x->outlet_stddev, atom_getfloat(argv + 1));
    outlet_float(x->outlet_rating, atom_getfloat(argv));
    outlet_float(x->outlet_barlen, atom_getfloat(argv + 4));
}

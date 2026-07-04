#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "ext_path.h"
#include "z_dsp.h"
#include "cumulative_transience.h"
#include "../shared/async_worker.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_AUDIO_SECONDS 60
#define ANALYSIS_HOP_MS 100

typedef struct _analyze {
    t_pxobject obj;

    // Outlets
    void* outlet_list;      // Band, Score
    void* outlet_barlen;
    void* outlet_rating;
    void* outlet_stddev;
    void* outlet_contrast;
    void* outlet_peakstd;

    // Analyzer State
    TransientAnalyzer* analyzer;
    double sample_rate;

    // Circular Buffer for Audio
    float* audio_buffer;
    int audio_buffer_size;
    int audio_buffer_write_ptr;

    // Processing State
    int current_sample_count;
    int last_analysis_frame;
    int last_peak_frame[MAX_BANDS];

    // Async Worker
    t_async_worker* worker;
    t_critical lock;
    int invalidated;
    int pending_analysis;

    ChunkAnalysisResult* result_buffer;

} t_analyze;

void* analyze_new(t_symbol* s, long argc, t_atom* argv);
void analyze_free(t_analyze* x);
void analyze_worker_task(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_metrics(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_peak(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_dsp64(t_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void analyze_perform64(t_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam);
void analyze_assist(t_analyze* x, void* b, long m, long a, char* s);

static t_class* analyze_class;

void ext_main(void* r) {
    t_class* c = class_new("analyze~", (method)analyze_new, (method)analyze_free, sizeof(t_analyze), 0L, A_GIMME, 0);

    class_addmethod(c, (method)analyze_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)analyze_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    analyze_class = c;
}

void* analyze_new(t_symbol* s, long argc, t_atom* argv) {
    t_analyze* x = (t_analyze*)object_alloc(analyze_class);

    if (x) {
        dsp_setup((t_pxobject*)x, 1);

        // Outlets (Right-to-Left)
        x->outlet_peakstd = floatout(x);
        x->outlet_contrast = floatout(x);
        x->outlet_stddev = floatout(x);
        x->outlet_rating = floatout(x);
        x->outlet_barlen = floatout(x);
        x->outlet_list = listout(x);

        critical_new(&x->lock);

        x->analyzer = analyzer_create(1.0);
        x->worker = async_worker_create();

        x->audio_buffer = NULL;
        x->audio_buffer_size = 0;
        x->audio_buffer_write_ptr = 0;
        x->current_sample_count = 0;
        x->last_analysis_frame = 0;
        for(int i=0; i<MAX_BANDS; i++) x->last_peak_frame[i] = -1;

        x->invalidated = 0;
        x->pending_analysis = 0;
        x->sample_rate = 44100.0;
        x->result_buffer = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));
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

    if (x->result_buffer) free(x->result_buffer);

    free(x->audio_buffer);
    critical_free(x->lock);
}

void analyze_assist(t_analyze* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "(signal) Audio Input");
    } else {
        switch (a) {
            case 0: sprintf(s, "(list) Band, Score"); break;
            case 1: sprintf(s, "(float) Bar Length (ms)"); break;
            case 2: sprintf(s, "(float) Rating Score"); break;
            case 3: sprintf(s, "(float) Standard Deviation"); break;
            case 4: sprintf(s, "(float) Contrast Score"); break;
            case 5: sprintf(s, "(float) Highest Peak Deviation"); break;
        }
    }
}

void analyze_dsp64(t_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    x->sample_rate = samplerate;
    analyzer_set_sample_rate(x->analyzer, (int)samplerate);

    int new_size = (int)(samplerate * MAX_AUDIO_SECONDS);
    if (x->audio_buffer_size != new_size) {
        free(x->audio_buffer);
        x->audio_buffer = (float*)calloc(new_size, sizeof(float));
        x->audio_buffer_size = new_size;
        x->audio_buffer_write_ptr = 0;
        x->current_sample_count = 0;
    }

    dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)analyze_perform64, 0, NULL);
}

void analyze_perform64(t_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam) {
    double* in = ins[0];

    for (int i = 0; i < sampleframes; i++) {
        x->audio_buffer[x->audio_buffer_write_ptr] = (float)in[i];
        x->audio_buffer_write_ptr = (x->audio_buffer_write_ptr + 1) % x->audio_buffer_size;
        x->current_sample_count++;
    }

    int hop_samples = (int)(x->sample_rate * 0.001 * ANALYSIS_HOP_MS);
    if (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
        if (!x->pending_analysis) {
            x->pending_analysis = 1;
            async_worker_enqueue(x->worker, x, (method)analyze_worker_task, gensym("analyze"), 0, NULL);
            x->last_analysis_frame = x->current_sample_count;
        }
    }
}

void analyze_worker_task(t_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    if (x->invalidated || !x->analyzer) {
        x->pending_analysis = 0;
        return;
    }

    // Incremental Model: We send only the NEW 100ms hop to the C core.
    int hop_samples = (int)(x->sample_rate * 0.1);

    // The "current" 100ms hop we just filled in the circular buffer
    int hop_start_samples = x->last_analysis_frame - hop_samples;
    if (hop_start_samples < 0) {
        x->pending_analysis = 0;
        return;
    }

    float* hop_audio = (float*)malloc(sizeof(float) * hop_samples);
    if (!hop_audio) {
        x->pending_analysis = 0;
        return;
    }

    int read_ptr = (x->audio_buffer_write_ptr - (x->current_sample_count - hop_start_samples) + x->audio_buffer_size) % x->audio_buffer_size;
    for (int i = 0; i < hop_samples; i++) {
        hop_audio[i] = x->audio_buffer[read_ptr];
        read_ptr = (read_ptr + 1) % x->audio_buffer_size;
    }

    // Calculation of global frames for the C core's internal alignment
    int ms_samples = (int)(x->sample_rate * 0.001);

    // Buffer start frame in the C core's internal 15.2s cache.
    // If the cache is full, it's (active_start_frame - 15000).
    int active_start_samples = x->last_analysis_frame - hop_samples - (int)(x->sample_rate * 0.2);
    int active_start_frame = active_start_samples / ms_samples;

    int window_start_samples = active_start_samples - (int)(x->sample_rate * 15.0);
    if (window_start_samples < 0) window_start_samples = 0;
    int buffer_start_frame = window_start_samples / ms_samples;

    if (x->result_buffer && analyzer_analyze_chunk(x->analyzer, hop_audio, hop_samples, (int)x->sample_rate, buffer_start_frame, active_start_frame, x->result_buffer)) {

        for (int i = 0; i < x->result_buffer->peak_list.num_peaks; i++) {
            PeakResult* pr = &x->result_buffer->peak_list.peaks[i];

            t_atom out_args[2];
            atom_setlong(out_args, pr->band_idx);
            atom_setfloat(out_args + 1, pr->total_score);
            defer(x, (method)analyze_output_peak, NULL, 2, out_args);
        }

        t_atom out_args[5];
        atom_setfloat(out_args, x->result_buffer->metrics.rating);
        atom_setfloat(out_args + 1, x->result_buffer->metrics.std_dev);
        atom_setfloat(out_args + 2, x->result_buffer->metrics.contrast);
        atom_setfloat(out_args + 3, x->result_buffer->metrics.peak_std);
        atom_setfloat(out_args + 4, x->result_buffer->metrics.highest_peak_valid ? x->result_buffer->metrics.highest_peak_ms : -999.0);
        defer(x, (method)analyze_output_metrics, NULL, 5, out_args);
    }

    free(hop_audio);
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

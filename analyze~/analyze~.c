#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "ext_path.h"
#include "z_dsp.h"
#include "cumulative_transience.h"
#include "../shared/async_worker.h"
#include "../shared/visualize.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#define MAX_AUDIO_SECONDS 60
#define ANALYSIS_HOP_MS 100
#define MAX_ANALYZE_CHANNELS 1024

typedef struct _analyze_shared_buffer {
    t_object ob;
    t_symbol* name;
    SharedTransientBuffer* buffer;
    t_critical lock;
    int ref_count;
} t_analyze_shared_buffer;

static t_class* analyze_shared_buffer_class = NULL;

void analyze_shared_buffer_free(t_analyze_shared_buffer* x) {
    object_unregister(x);
    if (x->lock) {
        critical_free(x->lock);
        x->lock = NULL;
    }
    if (x->buffer) {
        free(x->buffer);
        x->buffer = NULL;
    }
}

void* analyze_shared_buffer_new(t_symbol* s, long argc, t_atom* argv) {
    t_analyze_shared_buffer* x = NULL;
    t_symbol* name = (argc > 0 && atom_gettype(argv) == A_SYM) ? atom_getsym(argv) : NULL;
    if (name && name != gensym("")) {
        t_class* sc = analyze_shared_buffer_class;
        if (!sc) sc = class_findbyname(CLASS_NOBOX, gensym("analyze_shared_buffer"));
        if (sc) {
            x = (t_analyze_shared_buffer*)object_alloc(sc);
            if (x) {
                x->name = name;
                x->buffer = (SharedTransientBuffer*)calloc(1, sizeof(SharedTransientBuffer));
                x->buffer->min_score_seen = DBL_MAX;
                x->buffer->max_score_seen = -DBL_MAX;
                x->buffer->max_peak = 1.0;
                critical_new(&x->lock);
                x->ref_count = 0;
                object_register(gensym("analyze_shared_buffer"), name, x);
            }
        }
    }
    return x;
}

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
    long weighted_bar;
    double tolerance;
    long visualize_enabled;
    int viz_port;
    int instance_id;

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
    long clear_sequence;

    char paused_channels[MAX_ANALYZE_CHANNELS + 1];

    ChunkAnalysisResult* result_buffer;

} t_analyze;

void* analyze_new(t_symbol* s, long argc, t_atom* argv);
void analyze_free(t_analyze* x);
void analyze_clear(t_analyze* x);
void analyze_pause(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_group_settor(t_analyze* x, void* attr, long argc, t_atom* argv);
void analyze_worker_task(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_metrics(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_peak(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_output_log(t_analyze* x, t_symbol* s, long argc, t_atom* argv);
void analyze_log(t_analyze* x, const char* fmt, ...);
void analyze_dsp64(t_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void analyze_perform64(t_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam);
void analyze_assist(t_analyze* x, void* b, long m, long a, char* s);

#ifndef MAX_PATH_CHARS
#define MAX_PATH_CHARS 1024
#endif

void get_object_directory(char *dir_out, size_t max_len) {
#if defined(WIN_VERSION) || defined(_WIN32)
    HMODULE hModule = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)get_object_directory, &hModule)) {
        char path[MAX_PATH_CHARS];
        DWORD len = GetModuleFileNameA(hModule, path, MAX_PATH_CHARS);
        if (len > 0 && len < MAX_PATH_CHARS) {
            char *last_sep = strrchr(path, '\\');
            if (!last_sep) {
                last_sep = strrchr(path, '/');
            }
            if (last_sep) {
                *last_sep = '\0';
                strncpy(dir_out, path, max_len);
                dir_out[max_len - 1] = '\0';
                return;
            }
        }
    }
#endif
    strncpy(dir_out, ".", max_len);
    dir_out[max_len - 1] = '\0';
}

static int file_exists(const char *filepath) {
#if defined(WIN_VERSION) || defined(_WIN32)
    DWORD dwAttrib = GetFileAttributesA(filepath);
    return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
#else
    FILE *f = fopen(filepath, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
#endif
}

static void get_visualizer_directory(char *dir_out, size_t max_len) {
    get_object_directory(dir_out, max_len);

    char test_path[MAX_PATH_CHARS * 2];
    snprintf(test_path, sizeof(test_path), "%s\\python\\transience_vis.py", dir_out);

    if (!file_exists(test_path)) {
        const char *fallback_dir = "D:\\[Library]\\[Documents]\\Max 8\\Library\\analyze~";
        strncpy(dir_out, fallback_dir, max_len);
        dir_out[max_len - 1] = '\0';
    }
}

static void launch_visualizer(t_analyze *x) {
    if (x->viz_port == 0) {
        x->viz_port = visualize_allocate_port(9001);

        char dir[MAX_PATH_CHARS];
        get_visualizer_directory(dir, sizeof(dir));
        char cmd[MAX_PATH_CHARS * 2];
        const char *grp = (x->group_name && x->group_name != gensym("")) ? x->group_name->s_name : "";
        t_symbol *s_name = object_attr_getsym(x, gensym("varname"));
        char scripting_name[128];
        if (s_name && s_name != gensym("")) {
            strncpy(scripting_name, s_name->s_name, sizeof(scripting_name));
            scripting_name[sizeof(scripting_name) - 1] = '\0';
        } else {
            snprintf(scripting_name, sizeof(scripting_name), "Instance #%d", x->instance_id);
        }
        snprintf(cmd, sizeof(cmd), "python \"%s\\python\\transience_vis.py\" --port %d --group \"%s\" --name \"%s\" --log %ld", dir, x->viz_port, grp, scripting_name, x->log_enabled);

#if defined(WIN_VERSION) || defined(_WIN32)
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            object_post((t_object *)x, "analyze~: Launched companion visualizer on port %d.", x->viz_port);
        } else {
            object_error((t_object *)x, "analyze~: Failed to launch companion visualizer: %s", cmd);
        }
#endif
    }
}

t_max_err analyze_attr_set_visualize(t_analyze *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        long prev = x->visualize_enabled;
        x->visualize_enabled = atom_getlong(av);
        if (x->visualize_enabled && !prev) {
            launch_visualizer(x);
        }
    }
    return MAX_ERR_NONE;
}

static t_class* analyze_class;

void ext_main(void* r) {
    t_class* sc = class_findbyname(CLASS_NOBOX, gensym("analyze_shared_buffer"));
    if (!sc) {
        sc = class_new("analyze_shared_buffer", (method)analyze_shared_buffer_new, (method)analyze_shared_buffer_free, sizeof(t_analyze_shared_buffer), 0L, A_GIMME, 0);
        class_register(CLASS_NOBOX, sc);
    }
    analyze_shared_buffer_class = sc;

    t_class* c = class_new("analyze~", (method)analyze_new, (method)analyze_free, sizeof(t_analyze), 0L, A_GIMME, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_analyze, log_enabled);
    CLASS_ATTR_FILTER_CLIP(c, "log", 0, 1);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "checkbox", "Log Diagnostics");

    CLASS_ATTR_SYM(c, "group", 0, t_analyze, group_name);
    CLASS_ATTR_ACCESSORS(c, "group", NULL, (method)analyze_group_settor);
    CLASS_ATTR_LABEL(c, "group", 0, "Shared Group Name");

    CLASS_ATTR_LONG(c, "weighted_bar", 0, t_analyze, weighted_bar);
    CLASS_ATTR_FILTER_CLIP(c, "weighted_bar", 0, 1);
    CLASS_ATTR_STYLE_LABEL(c, "weighted_bar", 0, "checkbox", "Weighted Bar Length Calculation");
    CLASS_ATTR_DEFAULT(c, "weighted_bar", 0, "1");

    CLASS_ATTR_DOUBLE(c, "tolerance", 0, t_analyze, tolerance);
    CLASS_ATTR_FILTER_CLIP(c, "tolerance", 0.0, 5000.0);
    CLASS_ATTR_LABEL(c, "tolerance", 0, "Tolerance (ms)");
    CLASS_ATTR_DEFAULT(c, "tolerance", 0, "29.0");

    CLASS_ATTR_LONG(c, "visualize", 0, t_analyze, visualize_enabled);
    CLASS_ATTR_FILTER_CLIP(c, "visualize", 0, 1);
    CLASS_ATTR_LABEL(c, "visualize", 0, "Enable Real-Time Visualization");
    CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)analyze_attr_set_visualize);
    CLASS_ATTR_DEFAULT(c, "visualize", 0, "0");

    class_addmethod(c, (method)analyze_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)analyze_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)analyze_clear, "clear", 0);
    class_addmethod(c, (method)analyze_pause, "pause", A_GIMME, 0);

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
        x->clear_sequence = 0;
        x->log_enabled = 0;
        x->weighted_bar = 1;
        x->tolerance = 29.0;
        x->sample_rate = 44100.0;
        x->visualize_enabled = 0;
        x->viz_port = 0;
        x->instance_id = (int)(rand() % 900000 + 100000);
        memset(x->paused_channels, 0, sizeof(x->paused_channels));
        x->result_buffer = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));

        visualize_init();

        attr_args_process(x, argc, argv);

        if (!x->analyzer) {
            x->analyzer = analyzer_create(1.0, NULL, x->lock, (ct_lock_func)critical_enter, (ct_lock_func)critical_exit);
        }
        if (x->analyzer) {
            x->analyzer->tolerance = x->tolerance;
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
        t_analyze_shared_buffer* entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
        if (entry) {
            entry->ref_count--;
            if (entry->ref_count <= 0) {
                object_free(entry);
            }
        }
    }

    if (x->result_buffer) free(x->result_buffer);

    free(x->audio_buffer);
    free(x->clock_buffer);
    critical_free(x->lock);

    if (x->viz_port > 0) {
        visualize_close_port(x->viz_port);
    }

    visualize_cleanup();
}

void analyze_clear(t_analyze* x) {
    if (x->worker) {
        async_worker_drain(x->worker);
    }

    critical_enter(x->lock);
    x->clear_sequence++;
    if (x->analyzer) {
        analyzer_clear(x->analyzer);
    }
    if (x->audio_buffer) {
        memset(x->audio_buffer, 0, sizeof(float) * x->audio_buffer_size);
    }
    if (x->clock_buffer) {
        memset(x->clock_buffer, 0, sizeof(double) * x->audio_buffer_size);
    }
    x->audio_buffer_write_ptr = 0;
    x->current_sample_count = 0;
    x->last_analysis_frame = 0;
    x->pending_analysis = 0;
    for (int i = 0; i < MAX_BANDS; i++) {
        x->last_peak_frame[i] = -1;
    }
    analyze_log(x, "cleared internal state");

    if (x->visualize_enabled && x->viz_port > 0) {
        char json_buf[512];
        const char *grp = (x->group_name && x->group_name != gensym("")) ? x->group_name->s_name : "";
        t_symbol *s_name = object_attr_getsym(x, gensym("varname"));
        char scripting_name[128];
        if (s_name && s_name != gensym("")) {
            strncpy(scripting_name, s_name->s_name, sizeof(scripting_name));
            scripting_name[sizeof(scripting_name) - 1] = '\0';
        } else {
            snprintf(scripting_name, sizeof(scripting_name), "Instance #%d", x->instance_id);
        }
        snprintf(json_buf, sizeof(json_buf), "{\"type\":\"analyze\",\"event\":\"clear\",\"group\":\"%s\",\"scripting_name\":\"%s\",\"rating\":0.0}", grp, scripting_name);
        visualize_to_port(x, x->viz_port, "analyze", json_buf);
    }

    critical_exit(x->lock);
}

void analyze_group_settor(t_analyze* x, void* attr, long argc, t_atom* argv) {
    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        t_symbol* name = atom_getsym(argv);
        if (name != x->group_name) {
            if (x->analyzer) {
                object_error((t_object*)x, "cannot change @group after initialization");
                return;
            }
            if (x->group_name && x->group_name != gensym("")) {
                t_analyze_shared_buffer* old_entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
                if (old_entry) {
                    old_entry->ref_count--;
                    if (old_entry->ref_count <= 0) {
                        object_free(old_entry);
                    }
                }
            }
            x->group_name = name;
            if (x->group_name != gensym("")) {
                t_analyze_shared_buffer* entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
                if (!entry) {
                    t_atom a;
                    atom_setsym(&a, x->group_name);
                    object_new_typed(CLASS_NOBOX, gensym("analyze_shared_buffer"), 1, &a);
                    entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
                }
                if (entry) {
                    entry->ref_count++;
                    x->analyzer = analyzer_create(1.0, entry->buffer, entry->lock, (ct_lock_func)critical_enter, (ct_lock_func)critical_exit);
                }
            }
        }
    }
}

void analyze_pause(t_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    char new_mask[MAX_ANALYZE_CHANNELS + 1];
    memset(new_mask, 0, sizeof(new_mask));

    for (long i = 0; i < argc; i++) {
        long chan = 0;
        if (atom_gettype(argv + i) == A_LONG) {
            chan = atom_getlong(argv + i);
        } else if (atom_gettype(argv + i) == A_FLOAT) {
            chan = (long)atom_getfloat(argv + i);
        } else if (atom_gettype(argv + i) == A_SYM) {
            t_symbol *sym = atom_getsym(argv + i);
            if (sym) {
                char *endptr = NULL;
                chan = strtol(sym->s_name, &endptr, 10);
                if (endptr && *endptr != '\0') {
                    chan = 0;
                }
            }
        }
        if (chan >= 1 && chan <= MAX_ANALYZE_CHANNELS) {
            new_mask[chan] = 1;
        }
    }

    critical_enter(x->lock);
    memcpy(x->paused_channels, new_mask, sizeof(x->paused_channels));
    critical_exit(x->lock);
    analyze_log(x, "updated pause state");
}

void analyze_assist(t_analyze* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(signal) Audio Input, (messages) clear, pause"); break;
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

    if (x->visualize_enabled) {
        launch_visualizer(x);
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
        critical_enter(x->lock);
        x->pending_analysis = 0;
        critical_exit(x->lock);
        return;
    }

    critical_enter(x->lock);
    long start_seq = x->clear_sequence;
    if (x->analyzer) {
        x->analyzer->tolerance = x->tolerance;
    }
    critical_exit(x->lock);

    int hop_samples = (int)(x->sample_rate * 0.1);
    int ms_samples = (int)(x->sample_rate * 0.001);

    int hops_processed = 0;
    while (1) {
        critical_enter(x->lock);
        if (x->invalidated || x->clear_sequence != start_seq || x->current_sample_count < x->last_analysis_frame + hop_samples) {
            critical_exit(x->lock);
            break;
        }
        long long cur_samples = x->current_sample_count;
        int cur_write_ptr = x->audio_buffer_write_ptr;
        long long target_analysis_frame = x->last_analysis_frame + hop_samples;
        critical_exit(x->lock);

        long long hop_start_samples = target_analysis_frame - hop_samples;

        float* hop_audio = (float*)malloc(sizeof(float) * hop_samples);
        if (!hop_audio) break;

        long long samples_ago = cur_samples - hop_start_samples;
        if (samples_ago < 0) samples_ago = 0;
        if (x->audio_buffer_size > 0 && samples_ago >= x->audio_buffer_size) samples_ago = x->audio_buffer_size - 1;

        int read_ptr = (int)((cur_write_ptr - samples_ago + x->audio_buffer_size) % x->audio_buffer_size);

        for (int i = 0; i < hop_samples; i++) {
            hop_audio[i] = x->audio_buffer[read_ptr];
            read_ptr = (read_ptr + 1) % x->audio_buffer_size;
        }

        int active_start_samples = (int)(target_analysis_frame - hop_samples - (int)(x->sample_rate * 0.2));
        int active_start_frame = active_start_samples / ms_samples;

        int window_start_samples = active_start_samples - (int)(x->sample_rate * 15.0);
        if (window_start_samples < 0) window_start_samples = 0;
        int buffer_start_frame = window_start_samples / ms_samples;

        int is_paused = 0;
        critical_enter(x->lock);
        is_paused = x->paused_channels[1];
        critical_exit(x->lock);

        if (is_paused) {
            if (x->analyzer) {
                analyzer_cleanup_snapshots(x->analyzer, active_start_frame);
                analyzer_push_audio(x->analyzer, hop_audio, hop_samples, (int)x->sample_rate);
            }
            free(hop_audio);

            critical_enter(x->lock);
            if (x->clear_sequence == start_seq) {
                x->last_analysis_frame = target_analysis_frame;
            } else {
                critical_exit(x->lock);
                break;
            }
            critical_exit(x->lock);
            continue;
        }

        if (x->result_buffer && analyzer_analyze_chunk(x->analyzer, hop_audio, hop_samples, (int)x->sample_rate, buffer_start_frame, active_start_frame, x->result_buffer)) {
            hops_processed++;

            for (int i = 0; i < x->result_buffer->peak_list.num_peaks; i++) {
                PeakResult* pr = &x->result_buffer->peak_list.peaks[i];

                if (x->clock_connected) {
                    long long peak_sample = (long long)pr->p_idx * ms_samples;
                    long long p_samples_ago = cur_samples - peak_sample;
                    if (p_samples_ago < 0) p_samples_ago = 0;
                    if (x->audio_buffer_size > 0 && p_samples_ago >= x->audio_buffer_size) p_samples_ago = x->audio_buffer_size - 1;
                    int clock_idx = (int)((cur_write_ptr - p_samples_ago + x->audio_buffer_size) % x->audio_buffer_size);
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

            double max_score = -1.0;
            int best_bar_length = 0;
            critical_enter(x->lock);
            if (x->analyzer) {
                for (int i = 0; i <= 5000; i++) {
                    int count = x->analyzer->bar_length_counts[i];
                    if (count > 0) {
                        double score = 0.0;
                        if (x->weighted_bar) {
                            score = (double)count * ((double)i / 5000.0);
                        } else {
                            score = (double)count;
                        }
                        if (score > max_score) {
                            max_score = score;
                            best_bar_length = i;
                        }
                    }
                }
            }
            critical_exit(x->lock);
            float barlen = (float)best_bar_length;

            atom_setfloat(out_args + 4, barlen);
            defer(x, (method)analyze_output_metrics, NULL, 5, out_args);

            if (x->visualize_enabled && x->viz_port > 0) {
                char *json_buf = (char *)malloc(131072);
                if (json_buf) {
                    char *ptr = json_buf;
                    int remaining = 131072;
                    int n;

                    double p_time = (double)target_analysis_frame / x->sample_rate;
                    const char *grp = (x->group_name && x->group_name != gensym("")) ? x->group_name->s_name : "";
                    t_symbol *s_name = object_attr_getsym(x, gensym("varname"));
                    char scripting_name[128];
                    if (s_name && s_name != gensym("")) {
                        strncpy(scripting_name, s_name->s_name, sizeof(scripting_name));
                        scripting_name[sizeof(scripting_name) - 1] = '\0';
                    } else {
                        snprintf(scripting_name, sizeof(scripting_name), "Instance #%d", x->instance_id);
                    }

                    n = snprintf(ptr, remaining, "{\"type\":\"analyze\",\"event\":\"update\",\"group\":\"%s\",\"scripting_name\":\"%s\",\"log\":%ld,\"time\":%.4f,", grp, scripting_name, x->log_enabled, p_time);
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    double hp_ms = x->result_buffer->metrics.highest_peak_valid ? x->result_buffer->metrics.highest_peak_ms : -999.0;
                    n = snprintf(ptr, remaining, "\"rating\":%.4f,\"std_dev\":%.4f,\"contrast\":%.4f,\"stability\":%.4f,\"max_peak_value\":%.4f,\"min_score_seen\":%.4f,\"max_score_seen\":%.4f,\"tolerance\":%.4f,\"highest_peak_ms\":%.4f,",
                                 x->result_buffer->metrics.rating,
                                 x->result_buffer->metrics.std_dev,
                                 x->result_buffer->metrics.contrast,
                                 x->result_buffer->metrics.stability_score,
                                 analyzer_get_max_peak(x->analyzer),
                                 x->result_buffer->metrics.min_score_seen,
                                 x->result_buffer->metrics.max_score_seen,
                                 x->tolerance,
                                 hp_ms);
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"global_smoothing_avg\":%.4f,", x->result_buffer->metrics.global_smoothing_avg);
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"smoothing_avgs\":[%.4f,%.4f,%.4f,%.4f],",
                                 x->result_buffer->metrics.band_smoothing_avgs[0],
                                 x->result_buffer->metrics.band_smoothing_avgs[1],
                                 x->result_buffer->metrics.band_smoothing_avgs[2],
                                 x->result_buffer->metrics.band_smoothing_avgs[3]);
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"flux\":[");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    for (int b = 0; b < 4; b++) {
                        n = snprintf(ptr, remaining, "[");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        for (int j = 0; j < 100; j++) {
                            n = snprintf(ptr, remaining, "%.4f%s", x->result_buffer->last_flux[b][j], (j == 99) ? "" : ",");
                            if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        }
                        n = snprintf(ptr, remaining, "]%s", (b == 3) ? "" : ",");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    }
                    n = snprintf(ptr, remaining, "],");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"smooth\":[");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    for (int b = 0; b < 4; b++) {
                        n = snprintf(ptr, remaining, "[");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        for (int j = 0; j < 100; j++) {
                            n = snprintf(ptr, remaining, "%.4f%s", x->result_buffer->last_dynamic_smoothing[b][j], (j == 99) ? "" : ",");
                            if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        }
                        n = snprintf(ptr, remaining, "]%s", (b == 3) ? "" : ",");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    }
                    n = snprintf(ptr, remaining, "],");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"prominence\":[");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    for (int b = 0; b < 4; b++) {
                        n = snprintf(ptr, remaining, "[");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        for (int j = 0; j < 100; j++) {
                            n = snprintf(ptr, remaining, "%.4f%s", x->result_buffer->last_prominence[b][j], (j == 99) ? "" : ",");
                            if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        }
                        n = snprintf(ptr, remaining, "]%s", (b == 3) ? "" : ",");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    }
                    n = snprintf(ptr, remaining, "],");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"accumulated_buffer\":[");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    double *acc_buf = analyzer_get_buffer(x->analyzer);
                    for (int i = 0; i < 5001; i++) {
                        n = snprintf(ptr, remaining, "%.4f%s", acc_buf[i], (i == 5000) ? "" : ",");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    }
                    n = snprintf(ptr, remaining, "],");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    n = snprintf(ptr, remaining, "\"peaks\":[");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    for (int i = 0; i < x->result_buffer->peak_list.num_peaks; i++) {
                        PeakResult *p_res = &x->result_buffer->peak_list.peaks[i];
                        n = snprintf(ptr, remaining, "{\"time\":%.4f,\"peak_val\":%.4f,\"total_score\":%.4f,\"band_idx\":%d,\"p_idx\":%d,\"qualifiers\":[",
                                     p_res->time, p_res->peak_val, p_res->total_score, p_res->band_idx, p_res->p_idx);
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        for (int q = 0; q < p_res->num_qualifiers; q++) {
                            n = snprintf(ptr, remaining, "{\"ms\":%.4f,\"val\":%.4f,\"orig_ms\":%.4f}%s",
                                         p_res->qualifiers[q].ms, p_res->qualifiers[q].val, p_res->qualifiers[q].orig_ms,
                                         (q == p_res->num_qualifiers - 1) ? "" : ",");
                            if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                        }
                        n = snprintf(ptr, remaining, "]}%s", (i == x->result_buffer->peak_list.num_peaks - 1) ? "" : ",");
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
                    }
                    n = snprintf(ptr, remaining, "]}");
                    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                    analyze_log(x, "queuing visualization telemetry packet (%ld bytes) to port %d", (long)strlen(json_buf), x->viz_port);
                    visualize_to_port(x, x->viz_port, "analyze", json_buf);
                    free(json_buf);
                }
            }
        }

        free(hop_audio);

        critical_enter(x->lock);
        if (x->clear_sequence == start_seq) {
            x->last_analysis_frame = target_analysis_frame;
        } else {
            critical_exit(x->lock);
            break;
        }
        critical_exit(x->lock);

        analyze_log(x, "processed chunk at %lld samples, num_peaks: %d", target_analysis_frame, x->result_buffer->peak_list.num_peaks);
    }

    if (hops_processed > 1) {
        analyze_log(x, "catch-up complete: processed %d hops", hops_processed);
    }

    critical_enter(x->lock);
    x->pending_analysis = 0;
    critical_exit(x->lock);
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

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

typedef struct _mc_analyze {
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

    // Multi-channel Channel Count
    long num_audio_chans;
    long num_clock_chans;

    // Visualizer ports per channel
    int* viz_ports;
    long allocated_viz_ports;
    int instance_id;

    // Shared buffer for local channel accumulation (when no @group is specified)
    SharedTransientBuffer* local_shared_buffer;
    t_critical local_shared_buffer_lock;

    // Analyzer State - Array of pointers
    TransientAnalyzer** analyzers;
    long analyzers_count;

    double sample_rate;

    // Circular Buffers: Array of float pointers for audio (one per channel), and one clock buffer
    float** audio_buffers;
    long allocated_audio_chans;
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

} t_mc_analyze;

void* mc_analyze_new(t_symbol* s, long argc, t_atom* argv);
void mc_analyze_free(t_mc_analyze* x);
void mc_analyze_clear(t_mc_analyze* x);
void mc_analyze_group_settor(t_mc_analyze* x, void* attr, long argc, t_atom* argv);
void mc_analyze_worker_task(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv);
void mc_analyze_output_metrics(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv);
void mc_analyze_output_peak(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv);
void mc_analyze_output_log(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv);
void mc_analyze_log(t_mc_analyze* x, const char* fmt, ...);
void mc_analyze_dsp64(t_mc_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void mc_analyze_perform64(t_mc_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam);
void mc_analyze_assist(t_mc_analyze* x, void* b, long m, long a, char* s);
long mc_analyze_inputchanged(t_mc_analyze* x, long index, long count);

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

static void launch_visualizers(t_mc_analyze *x) {
    char dir[MAX_PATH_CHARS];
    get_object_directory(dir, sizeof(dir));
    const char *grp = (x->group_name && x->group_name != gensym("")) ? x->group_name->s_name : "";
    t_symbol *s_name = object_attr_getsym(x, gensym("varname"));
    char scripting_name[128];
    if (s_name && s_name != gensym("")) {
        strncpy(scripting_name, s_name->s_name, sizeof(scripting_name));
        scripting_name[sizeof(scripting_name) - 1] = '\0';
    } else {
        snprintf(scripting_name, sizeof(scripting_name), "Instance #%d", x->instance_id);
    }

    long n_chans = x->num_audio_chans;
    if (n_chans <= 0) n_chans = 1;

    if (x->allocated_viz_ports < n_chans) {
        int *new_ports = (int*)calloc(n_chans, sizeof(int));
        if (x->viz_ports) {
            for (long i = 0; i < x->allocated_viz_ports; i++) {
                new_ports[i] = x->viz_ports[i];
            }
            free(x->viz_ports);
        }
        x->viz_ports = new_ports;
        x->allocated_viz_ports = n_chans;
    }

    for (long ch = 0; ch < n_chans; ch++) {
        if (x->viz_ports[ch] == 0) {
            x->viz_ports[ch] = visualize_allocate_port(9001);

            char cmd[MAX_PATH_CHARS * 2];
            snprintf(cmd, sizeof(cmd), "python \"%s\\python\\transience_vis.py\" --port %d --group \"%s\" --channel %ld --name \"%s\"", dir, x->viz_ports[ch], grp, ch, scripting_name);

#if defined(WIN_VERSION) || defined(_WIN32)
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));

            if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                object_post((t_object *)x, "mc.analyze~: Launched visualizer for Ch %ld on port %d.", ch, x->viz_ports[ch]);
            } else {
                object_error((t_object *)x, "mc.analyze~: Failed to launch visualizer for Ch %ld: %s", ch, cmd);
            }
#endif
        }
    }
}

t_max_err mc_analyze_attr_set_visualize(t_mc_analyze *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        long prev = x->visualize_enabled;
        x->visualize_enabled = atom_getlong(av);
        if (x->visualize_enabled && !prev) {
            launch_visualizers(x);
        }
    }
    return MAX_ERR_NONE;
}

static t_class* mc_analyze_class;

void ext_main(void* r) {
    t_class* sc = class_findbyname(CLASS_NOBOX, gensym("analyze_shared_buffer"));
    if (!sc) {
        sc = class_new("analyze_shared_buffer", (method)analyze_shared_buffer_new, (method)analyze_shared_buffer_free, sizeof(t_analyze_shared_buffer), 0L, A_GIMME, 0);
        class_register(CLASS_NOBOX, sc);
    }
    analyze_shared_buffer_class = sc;

    t_class* c = class_new("mc.analyze~", (method)mc_analyze_new, (method)mc_analyze_free, sizeof(t_mc_analyze), 0L, A_GIMME, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_mc_analyze, log_enabled);
    CLASS_ATTR_FILTER_CLIP(c, "log", 0, 1);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "checkbox", "Log Diagnostics");

    CLASS_ATTR_SYM(c, "group", 0, t_mc_analyze, group_name);
    CLASS_ATTR_ACCESSORS(c, "group", NULL, (method)mc_analyze_group_settor);
    CLASS_ATTR_LABEL(c, "group", 0, "Shared Group Name");

    CLASS_ATTR_LONG(c, "weighted_bar", 0, t_mc_analyze, weighted_bar);
    CLASS_ATTR_FILTER_CLIP(c, "weighted_bar", 0, 1);
    CLASS_ATTR_STYLE_LABEL(c, "weighted_bar", 0, "checkbox", "Weighted Bar Length Calculation");
    CLASS_ATTR_DEFAULT(c, "weighted_bar", 0, "1");

    CLASS_ATTR_DOUBLE(c, "tolerance", 0, t_mc_analyze, tolerance);
    CLASS_ATTR_FILTER_CLIP(c, "tolerance", 0.0, 5000.0);
    CLASS_ATTR_LABEL(c, "tolerance", 0, "Tolerance (ms)");
    CLASS_ATTR_DEFAULT(c, "tolerance", 0, "29.0");

    CLASS_ATTR_LONG(c, "visualize", 0, t_mc_analyze, visualize_enabled);
    CLASS_ATTR_FILTER_CLIP(c, "visualize", 0, 1);
    CLASS_ATTR_LABEL(c, "visualize", 0, "Enable Real-Time Visualization");
    CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)mc_analyze_attr_set_visualize);
    CLASS_ATTR_DEFAULT(c, "visualize", 0, "0");

    class_addmethod(c, (method)mc_analyze_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)mc_analyze_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)mc_analyze_clear, "clear", 0);
    class_addmethod(c, (method)mc_analyze_inputchanged, "inputchanged", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    mc_analyze_class = c;
}

long mc_analyze_inputchanged(t_mc_analyze* x, long index, long count) {
    return false;
}

void* mc_analyze_new(t_symbol* s, long argc, t_atom* argv) {
    t_mc_analyze* x = (t_mc_analyze*)object_alloc(mc_analyze_class);

    if (x) {
        dsp_setup((t_pxobject*)x, 2);
        x->obj.z_misc |= Z_MC_INLETS;

        x->outlet_log = outlet_new(x, NULL);        // Outlet 6
        x->outlet_peakstd = floatout(x);           // Outlet 5 (Stability)
        x->outlet_contrast = floatout(x);          // Outlet 4
        x->outlet_stddev = floatout(x);            // Outlet 3
        x->outlet_rating = floatout(x);            // Outlet 2
        x->outlet_barlen = floatout(x);            // Outlet 1
        x->outlet_list = listout(x);               // Outlet 0

        critical_new(&x->lock);

        x->group_name = gensym("");
        x->analyzers = NULL;
        x->analyzers_count = 0;
        x->viz_ports = NULL;
        x->allocated_viz_ports = 0;
        x->worker = async_worker_create();

        x->audio_buffers = NULL;
        x->allocated_audio_chans = 0;
        x->clock_buffer = NULL;
        x->audio_buffer_size = 0;
        x->audio_buffer_write_ptr = 0;
        x->current_sample_count = 0;
        x->last_analysis_frame = 0;
        for(int i=0; i<MAX_BANDS; i++) x->last_peak_frame[i] = -1;

        x->num_audio_chans = 1;
        x->num_clock_chans = 1;

        x->local_shared_buffer = NULL;

        x->invalidated = 0;
        x->pending_analysis = 0;
        x->log_enabled = 0;
        x->weighted_bar = 1;
        x->tolerance = 29.0;
        x->sample_rate = 44100.0;
        x->visualize_enabled = 0;
        x->instance_id = (int)(rand() % 900000 + 100000);
        x->result_buffer = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));

        visualize_init();

        attr_args_process(x, argc, argv);

        x->local_shared_buffer = (SharedTransientBuffer*)calloc(1, sizeof(SharedTransientBuffer));
        x->local_shared_buffer->min_score_seen = DBL_MAX;
        x->local_shared_buffer->max_score_seen = -DBL_MAX;
        x->local_shared_buffer->max_peak = 1.0;
        critical_new(&x->local_shared_buffer_lock);
    }
    return x;
}

void mc_analyze_free(t_mc_analyze* x) {
    dsp_free((t_pxobject*)x);

    critical_enter(x->lock);
    x->invalidated = 1;
    critical_exit(x->lock);

    if (x->worker) {
        async_worker_release(x->worker);
    }

    if (x->analyzers) {
        for (long i = 0; i < x->analyzers_count; i++) {
            if (x->analyzers[i]) {
                analyzer_destroy(x->analyzers[i]);
            }
        }
        free(x->analyzers);
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

    if (x->local_shared_buffer) {
        critical_free(x->local_shared_buffer_lock);
        free(x->local_shared_buffer);
    }

    if (x->result_buffer) free(x->result_buffer);

    if (x->audio_buffers) {
        for (long i = 0; i < x->allocated_audio_chans; i++) {
            free(x->audio_buffers[i]);
        }
        free(x->audio_buffers);
    }
    free(x->clock_buffer);

    if (x->viz_ports) {
        for (long i = 0; i < x->allocated_viz_ports; i++) {
            if (x->viz_ports[i] > 0) {
                visualize_close_port(x->viz_ports[i]);
            }
        }
        free(x->viz_ports);
    }

    critical_free(x->lock);

    visualize_cleanup();
}

void mc_analyze_clear(t_mc_analyze* x) {
    critical_enter(x->lock);
    if (x->analyzers) {
        for (long i = 0; i < x->analyzers_count; i++) {
            if (x->analyzers[i]) {
                analyzer_clear(x->analyzers[i]);
            }
        }
    }
    if (x->audio_buffers) {
        for (long ch = 0; ch < x->allocated_audio_chans; ch++) {
            if (x->audio_buffers[ch]) {
                memset(x->audio_buffers[ch], 0, sizeof(float) * x->audio_buffer_size);
            }
        }
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
    mc_analyze_log(x, "cleared internal state");
    critical_exit(x->lock);
}

void mc_analyze_group_settor(t_mc_analyze* x, void* attr, long argc, t_atom* argv) {
    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        t_symbol* name = atom_getsym(argv);
        if (name != x->group_name) {
            if (x->analyzers_count > 0) {
                object_error((t_object*)x, "cannot change @group after initialization or while DSP is running");
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
            if (x->group_name && x->group_name != gensym("")) {
                t_analyze_shared_buffer* entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
                if (!entry) {
                    t_atom a;
                    atom_setsym(&a, x->group_name);
                    object_new_typed(CLASS_NOBOX, gensym("analyze_shared_buffer"), 1, &a);
                    entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
                }
                if (entry) {
                    entry->ref_count++;
                }
            }
        }
    }
}

void mc_analyze_assist(t_mc_analyze* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(signal/multichannelsignal) Audio Input"); break;
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

void mc_analyze_log(t_mc_analyze* x, const char* fmt, ...) {
    if (x->log_enabled && x->outlet_log) {
        char buf[4096];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 4096, fmt, args);
        va_end(args);

        t_atom a;
        atom_setsym(&a, gensym(buf));
        defer(x, (method)mc_analyze_output_log, NULL, 1, &a);
    }
}

void mc_analyze_output_log(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    if (!x->invalidated && x->outlet_log) {
        outlet_anything(x->outlet_log, atom_getsym(argv), 0, NULL);
    }
}

void mc_analyze_dsp64(t_mc_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    x->sample_rate = samplerate;

    long num_audio_chans = (long)(intptr_t)object_method(dsp64, gensym("getnuminputchannels"), x, 0);
    long num_clock_chans = (long)(intptr_t)object_method(dsp64, gensym("getnuminputchannels"), x, 1);

    if (num_audio_chans <= 0) {
        num_audio_chans = 1;
    }

    x->num_audio_chans = num_audio_chans;
    x->num_clock_chans = num_clock_chans;
    x->clock_connected = (num_clock_chans > 0 && count[num_audio_chans]);

    int new_size = (int)(samplerate * MAX_AUDIO_SECONDS);

    critical_enter(x->lock);

    if (x->audio_buffer_size != new_size || x->allocated_audio_chans != num_audio_chans) {
        mc_analyze_log(x, "reallocating audio and clock buffers: %d samples, %ld channels", new_size, num_audio_chans);

        if (x->audio_buffers) {
            for (long i = 0; i < x->allocated_audio_chans; i++) {
                free(x->audio_buffers[i]);
            }
            free(x->audio_buffers);
        }
        free(x->clock_buffer);

        x->audio_buffers = (float**)calloc(num_audio_chans, sizeof(float*));
        for (long i = 0; i < num_audio_chans; i++) {
            x->audio_buffers[i] = (float*)calloc(new_size, sizeof(float));
        }
        x->clock_buffer = (double*)calloc(new_size, sizeof(double));

        x->audio_buffer_size = new_size;
        x->allocated_audio_chans = num_audio_chans;
        x->audio_buffer_write_ptr = 0;
        x->current_sample_count = 0;
        x->last_analysis_frame = 0;
    }

    if (x->allocated_viz_ports != num_audio_chans) {
        int *new_ports = (int*)calloc(num_audio_chans, sizeof(int));
        long min_ch = (x->allocated_viz_ports < num_audio_chans) ? x->allocated_viz_ports : num_audio_chans;

        if (x->viz_ports) {
            for (long i = 0; i < min_ch; i++) {
                new_ports[i] = x->viz_ports[i];
            }
            for (long i = min_ch; i < x->allocated_viz_ports; i++) {
                if (x->viz_ports[i] > 0) {
                    visualize_close_port(x->viz_ports[i]);
                }
            }
            free(x->viz_ports);
        }
        x->viz_ports = new_ports;
        x->allocated_viz_ports = num_audio_chans;
    }

    if (x->analyzers_count != num_audio_chans) {
        if (x->analyzers) {
            for (long i = 0; i < x->analyzers_count; i++) {
                if (x->analyzers[i]) {
                    analyzer_destroy(x->analyzers[i]);
                }
            }
            free(x->analyzers);
        }

        SharedTransientBuffer* shared_buf = NULL;
        t_critical shared_lock = NULL;

        if (x->group_name && x->group_name != gensym("")) {
            t_analyze_shared_buffer* entry = (t_analyze_shared_buffer*)object_findregistered(gensym("analyze_shared_buffer"), x->group_name);
            if (entry && entry->buffer) {
                shared_buf = entry->buffer;
                shared_lock = entry->lock;
            }
        }
        if (!shared_buf) {
            shared_buf = x->local_shared_buffer;
            shared_lock = x->local_shared_buffer_lock;
        }

        x->analyzers = (TransientAnalyzer**)calloc(num_audio_chans, sizeof(TransientAnalyzer*));
        for (long i = 0; i < num_audio_chans; i++) {
            x->analyzers[i] = analyzer_create(1.0, shared_buf, shared_lock, (ct_lock_func)critical_enter, (ct_lock_func)critical_exit);
        }
        x->analyzers_count = num_audio_chans;
    }

    for (long i = 0; i < x->analyzers_count; i++) {
        if (x->analyzers[i]) {
            analyzer_set_sample_rate(x->analyzers[i], (int)samplerate);
            x->analyzers[i]->tolerance = x->tolerance;
        }
    }

    if (x->visualize_enabled) {
        launch_visualizers(x);
    }

    critical_exit(x->lock);

    dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)mc_analyze_perform64, 0, NULL);
}

void mc_analyze_perform64(t_mc_analyze* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam) {
    for (int i = 0; i < sampleframes; i++) {
        for (long ch = 0; ch < x->num_audio_chans; ch++) {
            x->audio_buffers[ch][x->audio_buffer_write_ptr] = (float)ins[ch][i];
        }
        if (x->clock_connected) {
            x->clock_buffer[x->audio_buffer_write_ptr] = ins[x->num_audio_chans][i];
        } else {
            x->clock_buffer[x->audio_buffer_write_ptr] = 0.0;
        }
        x->audio_buffer_write_ptr = (x->audio_buffer_write_ptr + 1) % x->audio_buffer_size;
        x->current_sample_count++;
    }

    int hop_samples = (int)(x->sample_rate * 0.001 * ANALYSIS_HOP_MS);
    if (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
        if (!x->pending_analysis) {
            x->pending_analysis = 1;
            mc_analyze_log(x, "triggering worker task, current_sample_count: %lld, last_analysis_frame: %lld", x->current_sample_count, x->last_analysis_frame);
            async_worker_enqueue(x->worker, x, (method)mc_analyze_worker_task, gensym("analyze"), 0, NULL);
        }
    }
}

void mc_analyze_worker_task(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    if (x->invalidated || !x->analyzers) {
        x->pending_analysis = 0;
        return;
    }

    critical_enter(x->lock);
    long n_chans = x->num_audio_chans;
    for (long ch = 0; ch < n_chans; ch++) {
        if (x->analyzers[ch]) {
            x->analyzers[ch]->tolerance = x->tolerance;
        }
    }
    critical_exit(x->lock);

    int hop_samples = (int)(x->sample_rate * 0.1);
    int ms_samples = (int)(x->sample_rate * 0.001);

    int hops_processed = 0;
    while (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
        long long cur_samples = x->current_sample_count;
        int cur_write_ptr = x->audio_buffer_write_ptr;

        if (x->invalidated) break;

        long long target_analysis_frame = x->last_analysis_frame + hop_samples;
        int hop_start_samples = (int)(target_analysis_frame - hop_samples);

        int active_start_samples = (int)(target_analysis_frame - hop_samples - (int)(x->sample_rate * 0.2));
        int active_start_frame = active_start_samples / ms_samples;

        int window_start_samples = active_start_samples - (int)(x->sample_rate * 15.0);
        if (window_start_samples < 0) window_start_samples = 0;
        int buffer_start_frame = window_start_samples / ms_samples;

        float* hop_audio = (float*)malloc(sizeof(float) * hop_samples);
        if (!hop_audio) break;

        hops_processed++;

        for (long ch = 0; ch < n_chans; ch++) {
            if (x->invalidated) break;

            long long samples_ago = cur_samples - hop_start_samples;
            int read_ptr = (int)((cur_write_ptr - samples_ago + x->audio_buffer_size) % x->audio_buffer_size);

            for (int i = 0; i < hop_samples; i++) {
                hop_audio[i] = x->audio_buffers[ch][read_ptr];
                read_ptr = (read_ptr + 1) % x->audio_buffer_size;
            }

            if (x->result_buffer && analyzer_analyze_chunk(x->analyzers[ch], hop_audio, hop_samples, (int)x->sample_rate, buffer_start_frame, active_start_frame, x->result_buffer)) {
                for (int i = 0; i < x->result_buffer->peak_list.num_peaks; i++) {
                    PeakResult* pr = &x->result_buffer->peak_list.peaks[i];

                    if (x->clock_connected) {
                        long long peak_sample = (long long)pr->p_idx * ms_samples;
                        long long s_ago = cur_samples - peak_sample;
                        int clock_idx = (int)((cur_write_ptr - s_ago + x->audio_buffer_size) % x->audio_buffer_size);
                        double clock_val = x->clock_buffer[clock_idx];

                        t_atom out_args[3];
                        atom_setfloat(out_args, clock_val);
                        atom_setlong(out_args + 1, pr->band_idx);
                        atom_setfloat(out_args + 2, pr->total_score);
                        defer(x, (method)mc_analyze_output_peak, NULL, 3, out_args);
                    } else {
                        t_atom out_args[2];
                        atom_setlong(out_args, pr->band_idx);
                        atom_setfloat(out_args + 1, pr->total_score);
                        defer(x, (method)mc_analyze_output_peak, NULL, 2, out_args);
                    }
                }

                if (x->visualize_enabled && ch < x->allocated_viz_ports && x->viz_ports[ch] > 0) {
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

                        n = snprintf(ptr, remaining, "{\"type\":\"mc_analyze\",\"event\":\"update\",\"group\":\"%s\",\"scripting_name\":\"%s\",\"channel\":%ld,\"time\":%.4f,", grp, scripting_name, ch, p_time);
                        if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

                        double hp_ms = x->result_buffer->metrics.highest_peak_valid ? x->result_buffer->metrics.highest_peak_ms : -999.0;
                        n = snprintf(ptr, remaining, "\"rating\":%.4f,\"std_dev\":%.4f,\"contrast\":%.4f,\"stability\":%.4f,\"max_peak_value\":%.4f,\"min_score_seen\":%.4f,\"max_score_seen\":%.4f,\"tolerance\":%.4f,\"highest_peak_ms\":%.4f,",
                                     x->result_buffer->metrics.rating,
                                     x->result_buffer->metrics.std_dev,
                                     x->result_buffer->metrics.contrast,
                                     x->result_buffer->metrics.stability_score,
                                     analyzer_get_max_peak(x->analyzers[ch]),
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
                        double *acc_buf = analyzer_get_buffer(x->analyzers[ch]);
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

                        mc_analyze_log(x, "queuing visualization telemetry packet (%ld bytes) for channel %ld to port %d", (long)strlen(json_buf), ch, x->viz_ports[ch]);
                        visualize_to_port(x, x->viz_ports[ch], "mc_analyze", json_buf);
                        free(json_buf);
                    }
                }
            }
        }

        if (n_chans > 0 && x->analyzers[0] && x->result_buffer) {
            analyzer_update_metrics(x->analyzers[0], active_start_frame + 100, &x->result_buffer->metrics);

            t_atom out_args[5];
            atom_setfloat(out_args, x->result_buffer->metrics.rating);
            atom_setfloat(out_args + 1, x->result_buffer->metrics.std_dev);
            atom_setfloat(out_args + 2, x->result_buffer->metrics.contrast);
            atom_setfloat(out_args + 3, x->result_buffer->metrics.stability_score);

            double max_score = -1.0;
            int best_bar_length = 0;

            int combined_bar_length_counts[5001] = {0};
            critical_enter(x->lock);
            for (long ch = 0; ch < n_chans; ch++) {
                if (x->analyzers[ch]) {
                    for (int i = 0; i <= 5000; i++) {
                        combined_bar_length_counts[i] += x->analyzers[ch]->bar_length_counts[i];
                    }
                }
            }
            critical_exit(x->lock);

            for (int i = 0; i <= 5000; i++) {
                if (combined_bar_length_counts[i] > 0) {
                    double score = 0.0;
                    if (x->weighted_bar) {
                        score = (double)combined_bar_length_counts[i] * ((double)i / 5000.0);
                    } else {
                        score = (double)combined_bar_length_counts[i];
                    }
                    if (score > max_score) {
                        max_score = score;
                        best_bar_length = i;
                    }
                }
            }

            float barlen = (float)best_bar_length;
            atom_setfloat(out_args + 4, barlen);
            defer(x, (method)mc_analyze_output_metrics, NULL, 5, out_args);
        }

        free(hop_audio);
        x->last_analysis_frame = target_analysis_frame;
        mc_analyze_log(x, "processed chunk at %lld samples", target_analysis_frame);
    }

    if (hops_processed > 1) {
        mc_analyze_log(x, "catch-up complete: processed %d hops", hops_processed);
    }

    x->pending_analysis = 0;
}

void mc_analyze_output_peak(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    outlet_list(x->outlet_list, NULL, argc, argv);
}

void mc_analyze_output_metrics(t_mc_analyze* x, t_symbol* s, long argc, t_atom* argv) {
    outlet_float(x->outlet_peakstd, atom_getfloat(argv + 3));
    outlet_float(x->outlet_contrast, atom_getfloat(argv + 2));
    outlet_float(x->outlet_stddev, atom_getfloat(argv + 1));
    outlet_float(x->outlet_rating, atom_getfloat(argv));
    outlet_float(x->outlet_barlen, atom_getfloat(argv + 4));
}

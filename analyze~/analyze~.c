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

typedef TransientAnalyzer* (*analyzer_create_ptr)(double);
typedef void (*analyzer_destroy_ptr)(TransientAnalyzer*);
typedef int (*analyzer_process_peak_ptr)(TransientAnalyzer*, int, int, double, const float*, int, const int*, int, PeakResult*);
typedef void (*analyzer_update_metrics_ptr)(TransientAnalyzer*, int, AnalyzerMetrics*);
typedef int (*analyzer_analyze_audio_ptr)(const float*, int, int, FullAnalysisResult*);
typedef void (*analyzer_free_analysis_ptr)(FullAnalysisResult*);

typedef struct _analyze {
    t_pxobject obj;

    // Outlets
    void* outlet_list;      // Band, Score
    void* outlet_rating;
    void* outlet_stddev;
    void* outlet_contrast;
    void* outlet_peakstd;

    // DLL Function Pointers
    HINSTANCE hLib;
    analyzer_create_ptr analyzer_create;
    analyzer_destroy_ptr analyzer_destroy;
    analyzer_process_peak_ptr analyzer_process_peak;
    analyzer_update_metrics_ptr analyzer_update_metrics;
    analyzer_analyze_audio_ptr analyzer_analyze_audio;
    analyzer_free_analysis_ptr analyzer_free_analysis;

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
        x->outlet_list = listout(x);

        critical_new(&x->lock);

        x->hLib = NULL;
        x->analyzer = NULL;
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

        // Load DLL
        char dll_path[MAX_PATH_CHARS];
        short pathid = 0;
        t_fourcc type = 0;
        char filename[MAX_FILENAME_CHARS];
        strncpy(filename, "analyze~.mxe64", MAX_FILENAME_CHARS);

        if (locatefile_extended(filename, &pathid, &type, NULL, 0) == 0) {
            char absolute_path[MAX_PATH_CHARS];
            path_toabsolutesystempath(pathid, filename, absolute_path);
            char* last_slash = strrchr(absolute_path, '\\');
            if (!last_slash) last_slash = strrchr(absolute_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(dll_path, MAX_PATH_CHARS, "%s\\libtransience.dll", absolute_path);
            } else {
                strcpy(dll_path, "libtransience.dll");
            }
        } else {
            strcpy(dll_path, "libtransience.dll");
        }

        x->hLib = LoadLibrary(dll_path);
        if (x->hLib) {
            x->analyzer_create = (analyzer_create_ptr)GetProcAddress(x->hLib, "analyzer_create");
            x->analyzer_destroy = (analyzer_destroy_ptr)GetProcAddress(x->hLib, "analyzer_destroy");
            x->analyzer_process_peak = (analyzer_process_peak_ptr)GetProcAddress(x->hLib, "analyzer_process_peak");
            x->analyzer_update_metrics = (analyzer_update_metrics_ptr)GetProcAddress(x->hLib, "analyzer_update_metrics");
            x->analyzer_analyze_audio = (analyzer_analyze_audio_ptr)GetProcAddress(x->hLib, "analyzer_analyze_audio");
            x->analyzer_free_analysis = (analyzer_free_analysis_ptr)GetProcAddress(x->hLib, "analyzer_free_analysis");

            if (x->analyzer_create) {
                x->analyzer = x->analyzer_create(1.0);
            }
        } else {
            object_error((t_object*)x, "Could not load libtransience.dll at %s", dll_path);
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

    if (x->analyzer && x->analyzer_destroy) {
        x->analyzer_destroy(x->analyzer);
    }

    if (x->hLib) {
        FreeLibrary(x->hLib);
    }

    free(x->audio_buffer);
    critical_free(x->lock);
}

void analyze_assist(t_analyze* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "(signal) Audio Input");
    } else {
        switch (a) {
            case 0: sprintf(s, "(list) Band, Score"); break;
            case 1: sprintf(s, "(float) Rating Score"); break;
            case 2: sprintf(s, "(float) Standard Deviation"); break;
            case 3: sprintf(s, "(float) Contrast Score"); break;
            case 4: sprintf(s, "(float) Highest Peak Deviation"); break;
        }
    }
}

void analyze_dsp64(t_analyze* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    x->sample_rate = samplerate;

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
    if (x->invalidated || !x->analyzer || !x->analyzer_analyze_audio) {
        x->pending_analysis = 0;
        return;
    }

    // Linearize audio for the DLL (reduced to last 2 seconds for performance)
    int analysis_seconds = 2;
    int analysis_samples = (int)(x->sample_rate * analysis_seconds);
    if (analysis_samples > x->current_sample_count) analysis_samples = x->current_sample_count;

    float* linear_audio = (float*)malloc(sizeof(float) * analysis_samples);
    if (!linear_audio) {
        x->pending_analysis = 0;
        return;
    }
    int read_ptr = (x->audio_buffer_write_ptr - analysis_samples + x->audio_buffer_size) % x->audio_buffer_size;
    for (int i = 0; i < analysis_samples; i++) {
        linear_audio[i] = x->audio_buffer[read_ptr];
        read_ptr = (read_ptr + 1) % x->audio_buffer_size;
    }

    FullAnalysisResult result;
    if (x->analyzer_analyze_audio(linear_audio, analysis_samples, (int)x->sample_rate, &result)) {
        int current_global_frame = x->current_sample_count / (int)(x->sample_rate * 0.001);
        int window_start_frame = (x->current_sample_count - analysis_samples) / (int)(x->sample_rate * 0.001);

        for (int b = 0; b < MAX_BANDS; b++) {
            for (int i = 0; i < result.bands[b].num_peaks; i++) {
                int peak_rel_frame = result.bands[b].peaks[i];
                int peak_abs_frame = window_start_frame + peak_rel_frame;

                if (peak_abs_frame > x->last_peak_frame[b]) {
                    PeakResult pr;
                    double time = peak_abs_frame * 0.001;

                    // Collect all peaks for process_peak (DLL signature)
                    int total_peaks = 0;
                    for (int bb = 0; bb < MAX_BANDS; bb++) total_peaks += result.bands[bb].num_peaks;
                    int* all_valid_peaks = (int*)malloc(sizeof(int) * total_peaks);
                    if (all_valid_peaks) {
                        int curr = 0;
                        for (int bb = 0; bb < MAX_BANDS; bb++) {
                            for (int k = 0; k < result.bands[bb].num_peaks; k++) {
                                all_valid_peaks[curr++] = result.bands[bb].peaks[k];
                            }
                        }

                        if (x->analyzer_process_peak(x->analyzer, peak_rel_frame, b, time, result.bands[b].envelope, result.num_frames, all_valid_peaks, total_peaks, &pr)) {
                            t_atom out_args[2];
                            atom_setlong(out_args, b);
                            atom_setfloat(out_args + 1, pr.total_score);
                            defer(x, (method)analyze_output_peak, NULL, 2, out_args);
                        }
                        free(all_valid_peaks);
                    }
                    x->last_peak_frame[b] = peak_abs_frame;
                }
            }
        }

        AnalyzerMetrics metrics;
        x->analyzer_update_metrics(x->analyzer, result.num_frames - 1, &metrics);

        t_atom out_args[4];
        atom_setfloat(out_args, metrics.rating);
        atom_setfloat(out_args + 1, metrics.std_dev);
        atom_setfloat(out_args + 2, metrics.contrast);
        atom_setfloat(out_args + 3, metrics.peak_std);
        defer(x, (method)analyze_output_metrics, NULL, 4, out_args);

        x->analyzer_free_analysis(&result);
    }

    free(linear_audio);
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
}

#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "doubles_dsp.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>

typedef struct _doubles_worker_data {
    float *ref_samples;
    long long ref_frames;
    float *subj_samples;
    long long subj_frames;
    double ref_sr;
    double subj_sr;
    t_symbol *dest_buf_name;
    t_symbol *path_buf_name;
} t_doubles_worker_data;

typedef struct _doubles {
    t_object b_obj;
    void *status_outlet;
    void *log_outlet;
    long log;

    t_critical lock;
    t_systhread thread;
    t_qelem *qelem;
    long is_busy;

    // Background worker data
    double progress;
    char last_error[256];

    // For deferring results to main thread
    float *defer_samples;
    long long defer_new_frames;
    t_symbol *defer_dest_name;

    double *defer_path_mapping;
    int defer_path_len;
    t_symbol *defer_path_name;

    t_doubles_worker_data *current_wd;
} t_doubles;

void *doubles_new(t_symbol *s, long argc, t_atom *argv);
void doubles_free(t_doubles *x);
void doubles_assist(t_doubles *x, void *b, long m, long a, char *s);
void doubles_align(t_doubles *x, t_symbol *s, long argc, t_atom *argv);
void doubles_worker_thread(t_doubles *x);
void doubles_qfn(t_doubles *x);

static t_class *doubles_class;

void ext_main(void *r) {
    t_class *c = class_new("doubles~", (method)doubles_new, (method)doubles_free, sizeof(t_doubles), 0L, A_GIMME, 0);

    class_addmethod(c, (method)doubles_align, "align", A_GIMME, 0);
    class_addmethod(c, (method)doubles_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_doubles, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    doubles_class = c;
    common_symbols_init();
}

void *doubles_new(t_symbol *s, long argc, t_atom *argv) {
    t_doubles *x = (t_doubles *)object_alloc(doubles_class);

    if (x) {
        x->log = 0;
        critical_new(&x->lock);
        x->qelem = qelem_new((t_object *)x, (method)doubles_qfn);
        x->thread = NULL;
        x->is_busy = 0;
        x->progress = 0.0;
        x->last_error[0] = '\0';
        x->defer_samples = NULL;
        x->defer_path_mapping = NULL;
        x->current_wd = NULL;

        attr_args_process(x, argc, argv);

        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);
    }
    return x;
}

void doubles_free(t_doubles *x) {
    if (x->thread) {
        unsigned int status;
        systhread_join(x->thread, &status);
        x->thread = NULL;
    }
    if (x->qelem) {
        qelem_free(x->qelem);
        x->qelem = NULL;
    }
    if (x->defer_samples) {
        sysmem_freeptr(x->defer_samples);
    }
    if (x->defer_path_mapping) {
        free(x->defer_path_mapping);
    }
    critical_free(x->lock);
}

void doubles_align(t_doubles *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc < 3) {
        object_error((t_object *)x, "align requires 3 arguments: [reference_buffer] [subject_buffer] [destination_buffer]");
        return;
    }

    critical_enter(x->lock);
    if (x->is_busy) {
        object_error((t_object *)x, "Object is busy with a previous alignment operation.");
        critical_exit(x->lock);
        return;
    }

    t_symbol *ref_name = atom_getsym(argv);
    t_symbol *subj_name = atom_getsym(argv + 1);
    t_symbol *dest_name = atom_getsym(argv + 2);
    t_symbol *path_name = (argc > 3) ? atom_getsym(argv + 3) : _sym_nothing;

    t_buffer_ref *ref_ref = buffer_ref_new((t_object *)x, ref_name);
    t_buffer_ref *subj_ref = buffer_ref_new((t_object *)x, subj_name);
    t_buffer_obj *ref_obj = buffer_ref_getobject(ref_ref);
    t_buffer_obj *subj_obj = buffer_ref_getobject(subj_ref);

    if (!ref_obj || !subj_obj) {
        object_error((t_object *)x, "Input buffers not found");
        object_free(ref_ref);
        object_free(subj_ref);
        critical_exit(x->lock);
        return;
    }

    long long ref_frames = buffer_getframecount(ref_obj);
    long long subj_frames = buffer_getframecount(subj_obj);
    double sr = buffer_getsamplerate(ref_obj);

    if (ref_frames <= 0 || subj_frames <= 0) {
        object_error((t_object *)x, "Buffers must not be empty");
        object_free(ref_ref);
        object_free(subj_ref);
        critical_exit(x->lock);
        return;
    }

    t_doubles_worker_data *wd = (t_doubles_worker_data *)sysmem_newptrclear(sizeof(t_doubles_worker_data));
    if (!wd) {
        object_error((t_object *)x, "Memory allocation failed");
        object_free(ref_ref);
        object_free(subj_ref);
        critical_exit(x->lock);
        return;
    }

    wd->ref_samples = (float *)sysmem_newptr(ref_frames * sizeof(float));
    wd->subj_samples = (float *)sysmem_newptr(subj_frames * sizeof(float));
    if (!wd->ref_samples || !wd->subj_samples) {
        object_error((t_object *)x, "Memory allocation failed for audio samples");
        if (wd->ref_samples) sysmem_freeptr(wd->ref_samples);
        if (wd->subj_samples) sysmem_freeptr(wd->subj_samples);
        sysmem_freeptr(wd);
        object_free(ref_ref);
        object_free(subj_ref);
        critical_exit(x->lock);
        return;
    }

    float *ref_ext = buffer_locksamples(ref_obj);
    if (ref_ext) {
        long chans = buffer_getchannelcount(ref_obj);
        for(long long i=0; i<ref_frames; i++) wd->ref_samples[i] = ref_ext[i * chans];
        buffer_unlocksamples(ref_obj);
    }

    float *subj_ext = buffer_locksamples(subj_obj);
    if (subj_ext) {
        long chans = buffer_getchannelcount(subj_obj);
        for(long long i=0; i<subj_frames; i++) wd->subj_samples[i] = subj_ext[i * chans];
        buffer_unlocksamples(subj_obj);
    }

    wd->ref_frames = ref_frames;
    wd->subj_frames = subj_frames;
    wd->ref_sr = sr;
    wd->subj_sr = buffer_getsamplerate(subj_obj);
    wd->dest_buf_name = dest_name;
    wd->path_buf_name = path_name;

    object_free(ref_ref);
    object_free(subj_ref);

    x->is_busy = 1;
    x->progress = 0.0;
    x->last_error[0] = '\0';
    x->current_wd = wd;

    systhread_create((method)doubles_worker_thread, x, 0, 0, 0, &x->thread);
    critical_exit(x->lock);
}

void doubles_worker_thread(t_doubles *x) {
    critical_enter(x->lock);
    t_doubles_worker_data *wd = x->current_wd;
    critical_exit(x->lock);

    if (!wd) {
        systhread_exit(0);
        return;
    }

    // MFCC Parameters
    int win_size = 1024;
    int hop_size = 256;
    int fft_size = 1024;
    int num_filters = 26;
    int num_ceps = 13;

    t_mel_filterbank *mfb_ref = mel_filterbank_init(num_filters, fft_size, wd->ref_sr, 0, wd->ref_sr/2);
    t_mel_filterbank *mfb_subj = mel_filterbank_init(num_filters, fft_size, wd->subj_sr, 0, wd->subj_sr/2);

    int ref_mfcc_len = (int)((wd->ref_frames - win_size) / hop_size) + 1;
    int subj_mfcc_len = (int)((wd->subj_frames - win_size) / hop_size) + 1;

    double **ref_mfccs = (double **)malloc(ref_mfcc_len * sizeof(double *));
    double **subj_mfccs = (double **)malloc(subj_mfcc_len * sizeof(double *));

    if (!mfb_ref || !mfb_subj || !ref_mfccs || !subj_mfccs) {
        critical_enter(x->lock);
        snprintf(x->last_error, 256, "Memory allocation failed in background thread");
        x->progress = 1.0;
        critical_exit(x->lock);
        if (ref_mfccs) free(ref_mfccs);
        if (subj_mfccs) free(subj_mfccs);
        if (mfb_ref) mel_filterbank_free(mfb_ref);
        if (mfb_subj) mel_filterbank_free(mfb_subj);
        goto cleanup;
    }

    for (int i = 0; i < ref_mfcc_len; i++) {
        ref_mfccs[i] = (double *)malloc(num_ceps * sizeof(double));
        if (!ref_mfccs[i]) break;
        double *segment = (double *)malloc(win_size * sizeof(double));
        if (segment) {
            for(int j=0; j<win_size; j++) segment[j] = wd->ref_samples[i * hop_size + j];
            calculate_mfcc(segment, win_size, fft_size, mfb_ref, num_ceps, ref_mfccs[i]);
            free(segment);
        }

        if (i % 100 == 0) {
            critical_enter(x->lock);
            x->progress = 0.3 * (double)i / ref_mfcc_len;
            critical_exit(x->lock);
            qelem_set(x->qelem);
        }
    }

    for (int i = 0; i < subj_mfcc_len; i++) {
        subj_mfccs[i] = (double *)malloc(num_ceps * sizeof(double));
        if (!subj_mfccs[i]) break;
        double *segment = (double *)malloc(win_size * sizeof(double));
        if (segment) {
            for(int j=0; j<win_size; j++) segment[j] = wd->subj_samples[i * hop_size + j];
            calculate_mfcc(segment, win_size, fft_size, mfb_subj, num_ceps, subj_mfccs[i]);
            free(segment);
        }

        if (i % 100 == 0) {
            critical_enter(x->lock);
            x->progress = 0.3 + 0.3 * (double)i / subj_mfcc_len;
            critical_exit(x->lock);
            qelem_set(x->qelem);
        }
    }

    // Apply Cepstral Mean Subtraction (CMS)
    normalize_mfccs(ref_mfccs, ref_mfcc_len, num_ceps);
    normalize_mfccs(subj_mfccs, subj_mfcc_len, num_ceps);

    // Transient Detection
    double *ref_transients = (double *)calloc(ref_mfcc_len, sizeof(double));
    double *subj_transients = (double *)calloc(subj_mfcc_len, sizeof(double));
    if (ref_transients && subj_transients) {
        detect_transients(wd->ref_samples, wd->ref_frames, win_size, hop_size, ref_transients);
        detect_transients(wd->subj_samples, wd->subj_frames, win_size, hop_size, subj_transients);
    }

    t_dtw_path *path = dtw_calculate(ref_mfccs, ref_mfcc_len, subj_mfccs, subj_mfcc_len, num_ceps, ref_transients, subj_transients);
    if (!path) {
        critical_enter(x->lock);
        snprintf(x->last_error, 256, "DTW calculation failed");
        x->progress = 1.0;
        critical_exit(x->lock);
    } else {
        critical_enter(x->lock);
        x->progress = 0.7;
        critical_exit(x->lock);
        qelem_set(x->qelem);

        // Path analysis for warnings
        for (int i = 1; i < path->length; i++) {
            int dr = path->points[i].ref_idx - path->points[i-1].ref_idx;
            int ds = path->points[i].subj_idx - path->points[i-1].subj_idx;

            // If stretching more than 4x (arbitrary threshold for 'extreme')
            if (dr > 0 && ds == 0) {
                // Potential repeat/stretch
                // We'd need to count consecutive ones, but let's look for mapping slope instead
            }
        }

        int mapping_len = 0;
        double *mapping = dtw_path_to_mapping(path, &mapping_len);

        // Analyze mapping for extreme warp factors
        int consecutive_repeats = 0;
        int max_repeats = 0;
        int max_repeat_idx = 0;
        for (int i = 1; i < mapping_len; i++) {
            if (mapping[i] == mapping[i-1]) {
                consecutive_repeats++;
                if (consecutive_repeats > max_repeats) {
                    max_repeats = consecutive_repeats;
                    max_repeat_idx = i;
                }
            } else {
                consecutive_repeats = 0;
            }
        }

        if (max_repeats > 10) { // e.g., more than 10 frames (~250ms)
            critical_enter(x->lock);
            double timestamp_ms = (max_repeat_idx * hop_size * 1000.0) / wd->ref_sr;
            snprintf(x->last_error, 256, "Warning: Extreme stretching detected at %.2f ms (%d consecutive frames)", timestamp_ms, max_repeats);
            critical_exit(x->lock);
        }

        float *dest_samples = (float *)sysmem_newptrclear(wd->ref_frames * sizeof(float));
        if (dest_samples) {
            wsola_process(wd->ref_samples, wd->ref_frames, wd->subj_samples, wd->subj_frames, dest_samples, path, hop_size, win_size);

            critical_enter(x->lock);
            x->progress = 0.9;
            x->defer_samples = dest_samples;
            x->defer_new_frames = wd->ref_frames;
            x->defer_dest_name = wd->dest_buf_name;

            if (wd->path_buf_name != _sym_nothing) {
                // Normalize mapping to [-1, 1] for visualization
                if (subj_mfcc_len > 1) {
                    for (int i = 0; i < mapping_len; i++) {
                        mapping[i] = (2.0 * mapping[i] / (subj_mfcc_len - 1)) - 1.0;
                    }
                } else {
                    for (int i = 0; i < mapping_len; i++) {
                        mapping[i] = 0.0;
                    }
                }
                x->defer_path_mapping = mapping;
                x->defer_path_len = mapping_len;
                x->defer_path_name = wd->path_buf_name;
                mapping = NULL; // qfn will free
            }
            critical_exit(x->lock);
        } else {
            critical_enter(x->lock);
            snprintf(x->last_error, 256, "Synthesis memory allocation failed");
            x->progress = 1.0;
            critical_exit(x->lock);
        }
        if (mapping) free(mapping);
        dtw_path_free(path);
    }

    if (ref_transients) free(ref_transients);
    if (subj_transients) free(subj_transients);

    // Cleanup
    if (ref_mfccs) {
        for(int i=0; i<ref_mfcc_len; i++) if (ref_mfccs[i]) free(ref_mfccs[i]);
        free(ref_mfccs);
    }
    if (subj_mfccs) {
        for(int i=0; i<subj_mfcc_len; i++) if (subj_mfccs[i]) free(subj_mfccs[i]);
        free(subj_mfccs);
    }
    mel_filterbank_free(mfb_ref);
    mel_filterbank_free(mfb_subj);

    critical_enter(x->lock);
    x->progress = 1.0;
    critical_exit(x->lock);

cleanup:
    if (wd) {
        sysmem_freeptr(wd->ref_samples);
        sysmem_freeptr(wd->subj_samples);
        sysmem_freeptr(wd);
        x->current_wd = NULL;
    }
    qelem_set(x->qelem);
    systhread_exit(0);
}

void doubles_qfn(t_doubles *x) {
    critical_enter(x->lock);
    double p = x->progress;
    char err[256];
    strncpy(err, x->last_error, 256);
    float *samples = x->defer_samples;
    long long frames = x->defer_new_frames;
    t_symbol *dest_name = x->defer_dest_name;
    double *mapping = x->defer_path_mapping;
    int mapping_len = x->defer_path_len;
    t_symbol *mapping_name = x->defer_path_name;
    critical_exit(x->lock);

    if (err[0]) {
        object_error((t_object *)x, "%s", err);
        critical_enter(x->lock);
        x->last_error[0] = '\0';
        critical_exit(x->lock);
    }

    if (samples) {
        t_buffer_ref *dest_ref = buffer_ref_new((t_object *)x, dest_name);
        t_buffer_obj *dest_obj = buffer_ref_getobject(dest_ref);
        if (dest_obj) {
            t_atom av;
            atom_setlong(&av, (t_atom_long)(frames * buffer_getchannelcount(dest_obj)));
            object_method_typed(dest_obj, gensym("sizeinsamps"), 1, &av, NULL);

            float *dest_ext = buffer_locksamples(dest_obj);
            if (dest_ext) {
                long chans = buffer_getchannelcount(dest_obj);
                for(long long i=0; i<frames; i++) {
                    for(long c=0; c<chans; c++) {
                        dest_ext[i * chans + c] = samples[i];
                    }
                }
                buffer_unlocksamples(dest_obj);
                buffer_setdirty(dest_obj);
            }
        }
        object_free(dest_ref);

        critical_enter(x->lock);
        sysmem_freeptr(x->defer_samples);
        x->defer_samples = NULL;
        critical_exit(x->lock);
    }

    if (mapping) {
        t_buffer_ref *path_ref = buffer_ref_new((t_object *)x, mapping_name);
        t_buffer_obj *path_obj = buffer_ref_getobject(path_ref);
        if (path_obj) {
            t_atom av;
            atom_setlong(&av, (t_atom_long)mapping_len);
            object_method_typed(path_obj, gensym("sizeinsamps"), 1, &av, NULL);

            float *path_ext = buffer_locksamples(path_obj);
            if (path_ext) {
                for (int i = 0; i < mapping_len; i++) {
                    path_ext[i] = (float)mapping[i];

                    // Simple warning for extreme stretching
                    if (i > 0) {
                        double delta = mapping[i] - mapping[i-1];
                        if (delta == 0 && x->log) {
                            // This frame is a repeat of previous subject frame
                        }
                    }
                }
                buffer_unlocksamples(path_obj);
                buffer_setdirty(path_obj);
            }
        }
        object_free(path_ref);

        critical_enter(x->lock);
        free(x->defer_path_mapping);
        x->defer_path_mapping = NULL;
        critical_exit(x->lock);
    }

    t_atom a;
    atom_setfloat(&a, (float)p);
    outlet_float(x->status_outlet, (float)p);

    if (p >= 1.0) {
        critical_enter(x->lock);
        x->is_busy = 0;
        if (x->thread) {
            unsigned int status;
            systhread_join(x->thread, &status);
            x->thread = NULL;
        }
        critical_exit(x->lock);

        t_atom dest;
        atom_setsym(&dest, dest_name ? dest_name : _sym_nothing);
        outlet_anything(x->status_outlet, gensym("finished"), 1, &dest);
        outlet_bang(x->status_outlet);
    }
}

void doubles_assist(t_doubles *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1 (anything): 'align [ref] [subj] [dest] (optional path_buf, normalized -1 to 1)' to start processing");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (float/bang): Progress (0.0-1.0), then 'finished [dest]' message and bang"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Logging Outlet"); break;
        }
    }
}

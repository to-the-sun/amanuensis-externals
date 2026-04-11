#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "doubles_dsp.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>

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
    t_symbol *ref_buf_name;
    t_symbol *subj_buf_name;
    t_symbol *dest_buf_name;
    double progress;
    char last_error[256];

    // For deferring buffer operations to main thread
    long long defer_new_frames;
    t_symbol *defer_dest_name;
    float *defer_samples;
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

    x->ref_buf_name = atom_getsym(argv);
    x->subj_buf_name = atom_getsym(argv + 1);
    x->dest_buf_name = atom_getsym(argv + 2);
    x->is_busy = 1;
    x->progress = 0.0;
    x->last_error[0] = '\0';
    if (x->defer_samples) {
        sysmem_freeptr(x->defer_samples);
        x->defer_samples = NULL;
    }

    systhread_create((method)doubles_worker_thread, x, 0, 0, 0, &x->thread);
    critical_exit(x->lock);
}

void doubles_worker_thread(t_doubles *x) {
    t_buffer_ref *ref_ref = buffer_ref_new((t_object *)x, x->ref_buf_name);
    t_buffer_ref *subj_ref = buffer_ref_new((t_object *)x, x->subj_buf_name);

    t_buffer_obj *ref_obj = buffer_ref_getobject(ref_ref);
    t_buffer_obj *subj_obj = buffer_ref_getobject(subj_ref);

    if (!ref_obj || !subj_obj) {
        critical_enter(x->lock);
        snprintf(x->last_error, 256, "One or more input buffers not found: %s, %s",
                 x->ref_buf_name->s_name, x->subj_buf_name->s_name);
        x->progress = 1.0;
        critical_exit(x->lock);
        goto cleanup;
    }

    long long ref_frames = buffer_getframecount(ref_obj);
    long long subj_frames = buffer_getframecount(subj_obj);
    double sr = buffer_getsamplerate(ref_obj);

    if (ref_frames == 0 || subj_frames == 0) {
        critical_enter(x->lock);
        snprintf(x->last_error, 256, "One or more buffers are empty");
        x->progress = 1.0;
        critical_exit(x->lock);
        goto cleanup;
    }

    float *ref_samples = (float *)sysmem_newptr(ref_frames * sizeof(float));
    float *subj_samples = (float *)sysmem_newptr(subj_frames * sizeof(float));
    if (!ref_samples || !subj_samples) {
        critical_enter(x->lock);
        snprintf(x->last_error, 256, "Memory allocation failed for audio samples");
        x->progress = 1.0;
        critical_exit(x->lock);
        if (ref_samples) sysmem_freeptr(ref_samples);
        if (subj_samples) sysmem_freeptr(subj_samples);
        goto cleanup;
    }

    float *ref_ext = buffer_locksamples(ref_obj);
    if (ref_ext) {
        long chans = buffer_getchannelcount(ref_obj);
        for(long long i=0; i<ref_frames; i++) ref_samples[i] = ref_ext[i * chans];
        buffer_unlocksamples(ref_obj);
    }

    float *subj_ext = buffer_locksamples(subj_obj);
    if (subj_ext) {
        long chans = buffer_getchannelcount(subj_obj);
        for(long long i=0; i<subj_frames; i++) subj_samples[i] = subj_ext[i * chans];
        buffer_unlocksamples(subj_obj);
    }

    // MFCC Parameters
    int win_size = 1024;
    int hop_size = 256;
    int fft_size = 1024;
    int num_filters = 26;
    int num_ceps = 13;

    t_mel_filterbank *mfb = mel_filterbank_init(num_filters, fft_size, sr, 0, sr/2);

    int ref_mfcc_len = (int)((ref_frames - win_size) / hop_size) + 1;
    int subj_mfcc_len = (int)((subj_frames - win_size) / hop_size) + 1;

    double **ref_mfccs = (double **)malloc(ref_mfcc_len * sizeof(double *));
    double **subj_mfccs = (double **)malloc(subj_mfcc_len * sizeof(double *));

    for (int i = 0; i < ref_mfcc_len; i++) {
        ref_mfccs[i] = (double *)malloc(num_ceps * sizeof(double));
        double *segment = (double *)malloc(win_size * sizeof(double));
        for(int j=0; j<win_size; j++) segment[j] = ref_samples[i * hop_size + j];
        calculate_mfcc(segment, win_size, fft_size, mfb, num_ceps, ref_mfccs[i]);
        free(segment);

        if (i % 50 == 0) {
            critical_enter(x->lock);
            x->progress = 0.3 * (double)i / ref_mfcc_len;
            critical_exit(x->lock);
            qelem_set(x->qelem);
        }
    }

    for (int i = 0; i < subj_mfcc_len; i++) {
        subj_mfccs[i] = (double *)malloc(num_ceps * sizeof(double));
        double *segment = (double *)malloc(win_size * sizeof(double));
        for(int j=0; j<win_size; j++) segment[j] = subj_samples[i * hop_size + j];
        calculate_mfcc(segment, win_size, fft_size, mfb, num_ceps, subj_mfccs[i]);
        free(segment);

        if (i % 50 == 0) {
            critical_enter(x->lock);
            x->progress = 0.3 + 0.3 * (double)i / subj_mfcc_len;
            critical_exit(x->lock);
            qelem_set(x->qelem);
        }
    }

    t_dtw_path *path = dtw_calculate(ref_mfccs, ref_mfcc_len, subj_mfccs, subj_mfcc_len, num_ceps);

    critical_enter(x->lock);
    x->progress = 0.7;
    critical_exit(x->lock);
    qelem_set(x->qelem);

    float *dest_samples = (float *)sysmem_newptrclear(ref_frames * sizeof(float));
    if (dest_samples) {
        wsola_process(ref_samples, ref_frames, subj_samples, subj_frames, dest_samples, path, hop_size, win_size);
    }

    critical_enter(x->lock);
    x->progress = 0.9;
    x->defer_samples = dest_samples;
    x->defer_new_frames = ref_frames;
    x->defer_dest_name = x->dest_buf_name;
    critical_exit(x->lock);
    qelem_set(x->qelem);

    // Cleanup
    for(int i=0; i<ref_mfcc_len; i++) free(ref_mfccs[i]);
    for(int i=0; i<subj_mfcc_len; i++) free(subj_mfccs[i]);
    free(ref_mfccs);
    free(subj_mfccs);
    dtw_path_free(path);
    mel_filterbank_free(mfb);
    sysmem_freeptr(ref_samples);
    sysmem_freeptr(subj_samples);

    critical_enter(x->lock);
    x->progress = 1.0;
    critical_exit(x->lock);

cleanup:
    object_free(ref_ref);
    object_free(subj_ref);
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
    critical_exit(x->lock);

    if (err[0]) {
        object_error((t_object *)x, "%s", err);
        critical_enter(x->lock);
        x->last_error[0] = '\0';
        critical_exit(x->lock);
    }

    // Handle deferred buffer update on main thread
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
            } else {
                object_error((t_object *)x, "Could not lock destination buffer for writing");
            }
        } else {
            object_error((t_object *)x, "Destination buffer %s not found", dest_name->s_name);
        }
        object_free(dest_ref);

        critical_enter(x->lock);
        sysmem_freeptr(x->defer_samples);
        x->defer_samples = NULL;
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
        atom_setsym(&dest, x->dest_buf_name);
        outlet_anything(x->status_outlet, gensym("finished"), 1, &dest);
        outlet_bang(x->status_outlet);
    }
}

void doubles_assist(t_doubles *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1 (anything): 'align [ref] [subj] [dest]' to start processing");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (float/bang): Progress (0.0-1.0), then 'finished [dest]' message and bang"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Logging Outlet"); break;
        }
    }
}

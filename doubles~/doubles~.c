#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "doubles_dsp.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>
#include <time.h>

typedef struct _doubles_worker_data {
    float **ref_samples;
    int ref_chans;
    long long ref_frames;
    float **subj_samples;
    int subj_chans;
    long long subj_frames;
    double ref_sr;
    double subj_sr;
    t_symbol *dest_buf_name;
    double targetbias;
    long long full_ref_frames;
    long long start_frame;
} t_doubles_worker_data;

typedef struct _doubles {
    t_object b_obj;
    void *status_outlet;
    void *function_outlet;
    void *log_outlet;
    long log;
    double targetbias;

    t_critical lock;
    t_systhread thread;
    t_qelem *qelem;
    long is_busy;

    // Background worker data
    double progress;
    char last_error[256];

    // For deferring results to main thread
    float **defer_samples;
    int defer_chans;
    long long defer_new_frames;
    long long defer_full_ref_frames;
    long long defer_start_frame;
    t_symbol *defer_dest_name;

    t_doubles_func_point *defer_func_points;
    int defer_func_points_count;

    t_doubles_worker_data *current_wd;
    t_symbol *last_dest_name;
} t_doubles;

void *doubles_new(t_symbol *s, long argc, t_atom *argv);
void doubles_free(t_doubles *x);
void doubles_assist(t_doubles *x, void *b, long m, long a, char *s);
void doubles_align(t_doubles *x, t_symbol *s, long argc, t_atom *argv);
void doubles_export(t_doubles *x, t_symbol *s, long argc, t_atom *argv);
void doubles_worker_thread(t_doubles *x);
void doubles_qfn(t_doubles *x);

static t_class *doubles_class;

void ext_main(void *r) {
    t_class *c = class_new("doubles~", (method)doubles_new, (method)doubles_free, sizeof(t_doubles), 0L, A_GIMME, 0);

    class_addmethod(c, (method)doubles_align, "align", A_GIMME, 0);
    class_addmethod(c, (method)doubles_export, "export", A_GIMME, 0);
    class_addmethod(c, (method)doubles_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_doubles, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_DOUBLE(c, "targetbias", 0, t_doubles, targetbias);
    CLASS_ATTR_LABEL(c, "targetbias", 0, "Target Bias");
    CLASS_ATTR_DEFAULT(c, "targetbias", 0, "0.01");
    CLASS_ATTR_SAVE(c, "targetbias", 0);

    class_register(CLASS_BOX, c);
    doubles_class = c;
    common_symbols_init();
}

void *doubles_new(t_symbol *s, long argc, t_atom *argv) {
    t_doubles *x = (t_doubles *)object_alloc(doubles_class);

    if (x) {
        x->log = 0;
        x->targetbias = 0.01;
        critical_new(&x->lock);
        x->qelem = qelem_new((t_object *)x, (method)doubles_qfn);
        x->thread = NULL;
        x->is_busy = 0;
        x->progress = 0.0;
        x->last_error[0] = '\0';
        x->defer_samples = NULL;
        x->defer_func_points = NULL;
        x->defer_func_points_count = 0;
        x->current_wd = NULL;
        x->last_dest_name = _sym_nothing;

        attr_args_process(x, argc, argv);

        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->function_outlet = outlet_new((t_object *)x, NULL);
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
        for (int i = 0; i < x->defer_chans; i++) {
            if (x->defer_samples[i]) sysmem_freeptr(x->defer_samples[i]);
        }
        sysmem_freeptr(x->defer_samples);
    }
    if (x->defer_func_points) {
        free(x->defer_func_points);
    }
    critical_free(x->lock);
}

void doubles_export(t_doubles *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc < 1) {
        object_error((t_object *)x, "export message requires a project path argument");
        return;
    }

    if (x->last_dest_name == _sym_nothing) {
        object_error((t_object *)x, "no destination buffer available for export (run 'align' first)");
        return;
    }

    t_symbol *project_path = atom_getsym(argv);
    t_object *coll = (t_object *)object_findregistered(gensym("nobox"), gensym("stems_info"));
    if (!coll) {
        object_error((t_object *)x, "could not find coll named 'stems_info'");
        return;
    }

    char found_filename[1024] = {0};
    bool found = false;

    // We need to find the entry where the first element of the list matches x->last_dest_name.
    // In the Max C API, calling the 'nth' method of a coll object can return a pointer to its data.
    for (int i = 1; i <= 2048; i++) {
        t_atom *atoms = (t_atom *)object_method(coll, gensym("nth"), i);
        if (atoms) {
            // Index 0 is the buffer name, Index 1 is the actual filename.
            if (atom_gettype(atoms) == A_SYM && atom_getsym(atoms) == x->last_dest_name) {
                if (atom_gettype(atoms + 1) == A_SYM) {
                    strncpy(found_filename, atom_getsym(atoms + 1)->s_name, 1023);
                    found = true;
                    break;
                }
            }
        }
    }

    // Fallback: search by key if buffer names are keys
    if (!found) {
        t_atom *atoms = (t_atom *)object_method(coll, gensym("sub"), x->last_dest_name);
        if (atoms && atom_gettype(atoms) == A_SYM) {
            strncpy(found_filename, atom_getsym(atoms)->s_name, 1023);
            found = true;
        }
    }

    if (!found) {
        object_error((t_object *)x, "could not find entry for buffer '%s' in coll 'stems_info'", x->last_dest_name->s_name);
        return;
    }

    // Filename modification logic
    // Format: name [timestamp].ext -> name (doubles) [new_timestamp].ext
    char modified_name[1024];
    char *bracket_start = strrchr(found_filename, '[');
    char *bracket_end = strrchr(found_filename, ']');

    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char timestamp[64];
    strftime(timestamp, 64, "%Y-%m-%d %H%M%S", tm_info);

    if (bracket_start && bracket_end && bracket_end > bracket_start) {
        // Part 1: everything before [
        size_t part1_len = bracket_start - found_filename;
        while (part1_len > 0 && found_filename[part1_len - 1] == ' ') part1_len--;

        char part1[1024];
        strncpy(part1, found_filename, part1_len);
        part1[part1_len] = '\0';

        // Part 2: everything after ]
        const char *part2 = bracket_end + 1;

        snprintf(modified_name, 1024, "%s (doubles) [%s]%s", part1, timestamp, part2);
    } else {
        // Fallback: insert before extension or at the end
        char *dot = strrchr(found_filename, '.');
        if (dot) {
            size_t base_len = dot - found_filename;
            char base[1024];
            strncpy(base, found_filename, base_len);
            base[base_len] = '\0';
            snprintf(modified_name, 1024, "%s (doubles) [%s]%s", base, timestamp, dot);
        } else {
            snprintf(modified_name, 1024, "%s (doubles) [%s]", found_filename, timestamp);
        }
    }

    // Construct full export path
    char full_path[2048];
    const char *pp = project_path->s_name;
    size_t pp_len = strlen(pp);
    snprintf(full_path, 2048, "%s%sSamples/Processed/Consolidate/%s",
             pp, (pp_len > 0 && (pp[pp_len-1] == '/' || pp[pp_len-1] == '\\')) ? "" : "/",
             modified_name);

    // Write destination buffer to disk
    t_buffer_ref *dest_ref = buffer_ref_new((t_object *)x, x->last_dest_name);
    t_buffer_obj *dest_obj = buffer_ref_getobject(dest_ref);
    if (dest_obj) {
        t_atom path_atom;
        atom_setsym(&path_atom, gensym(full_path));
        object_method_typed(dest_obj, gensym("write"), 1, &path_atom, NULL);
        object_post((t_object *)x, "exported buffer '%s' to: %s", x->last_dest_name->s_name, full_path);
    } else {
        object_error((t_object *)x, "destination buffer '%s' not found for export", x->last_dest_name->s_name);
    }
    object_free(dest_ref);
}

void doubles_align(t_doubles *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc < 3) {
        object_error((t_object *)x, "align requires 3 arguments: [reference_buffer] [subject_buffer] [destination_buffer] (optional [start_ms] [end_ms])");
        return;
    }

    t_symbol *ref_name = atom_getsym(argv);
    t_symbol *subj_name = atom_getsym(argv + 1);
    t_symbol *dest_name = atom_getsym(argv + 2);

    double start_ms = (argc >= 4) ? atom_getfloat(argv + 3) : 0.0;
    double end_ms = (argc >= 5) ? atom_getfloat(argv + 4) : -1.0;

    t_buffer_ref *ref_ref = buffer_ref_new((t_object *)x, ref_name);
    t_buffer_ref *subj_ref = buffer_ref_new((t_object *)x, subj_name);
    t_buffer_obj *ref_obj = buffer_ref_getobject(ref_ref);
    t_buffer_obj *subj_obj = buffer_ref_getobject(subj_ref);

    if (!ref_obj || !subj_obj) {
        object_error((t_object *)x, "Input buffers not found");
        if (ref_ref) object_free(ref_ref);
        if (subj_ref) object_free(subj_ref);
        return;
    }

    long long full_ref_frames = buffer_getframecount(ref_obj);
    long long full_subj_frames = buffer_getframecount(subj_obj);
    double ref_sr = buffer_getsamplerate(ref_obj);
    double subj_sr = buffer_getsamplerate(subj_obj);

    long long ref_start = (long long)(start_ms * ref_sr / 1000.0);
    long long ref_end = (end_ms < 0) ? full_ref_frames : (long long)(end_ms * ref_sr / 1000.0);
    long long subj_start = (long long)(start_ms * subj_sr / 1000.0);
    long long subj_end = (end_ms < 0) ? full_subj_frames : (long long)(end_ms * subj_sr / 1000.0);

    if (ref_start < 0) ref_start = 0;
    if (ref_end > full_ref_frames) ref_end = full_ref_frames;
    if (subj_start < 0) subj_start = 0;
    if (subj_end > full_subj_frames) subj_end = full_subj_frames;

    long long ref_frames = ref_end - ref_start;
    long long subj_frames = subj_end - subj_start;

    if (ref_frames < 1024 || subj_frames < 1024) {
        object_error((t_object *)x, "Analysis span must be at least 1024 samples long");
        object_free(ref_ref);
        object_free(subj_ref);
        return;
    }

    critical_enter(x->lock);
    if (x->is_busy) {
        object_error((t_object *)x, "Object is busy with a previous alignment operation.");
        critical_exit(x->lock);
        return;
    }

    int ref_chans = (int)buffer_getchannelcount(ref_obj);
    int subj_chans = (int)buffer_getchannelcount(subj_obj);

    t_doubles_worker_data *wd = (t_doubles_worker_data *)sysmem_newptrclear(sizeof(t_doubles_worker_data));
    if (!wd) {
        object_error((t_object *)x, "Memory allocation failed");
        object_free(ref_ref);
        object_free(subj_ref);
        critical_exit(x->lock);
        return;
    }

    wd->ref_samples = (float **)sysmem_newptrclear(ref_chans * sizeof(float *));
    wd->subj_samples = (float **)sysmem_newptrclear(subj_chans * sizeof(float *));
    if (!wd->ref_samples || !wd->subj_samples) {
        object_error((t_object *)x, "Memory allocation failed for audio channel pointers");
        if (wd->ref_samples) sysmem_freeptr(wd->ref_samples);
        if (wd->subj_samples) sysmem_freeptr(wd->subj_samples);
        sysmem_freeptr(wd);
        object_free(ref_ref);
        object_free(subj_ref);
        critical_exit(x->lock);
        return;
    }

    for (int c = 0; c < ref_chans; c++) {
        wd->ref_samples[c] = (float *)sysmem_newptr(ref_frames * sizeof(float));
        if (!wd->ref_samples[c]) {
            object_error((t_object *)x, "Memory allocation failed for ref channel %d", c);
            goto align_mem_err;
        }
    }
    for (int c = 0; c < subj_chans; c++) {
        wd->subj_samples[c] = (float *)sysmem_newptr(subj_frames * sizeof(float));
        if (!wd->subj_samples[c]) {
            object_error((t_object *)x, "Memory allocation failed for subj channel %d", c);
            goto align_mem_err;
        }
    }

    float *ref_ext = buffer_locksamples(ref_obj);
    if (ref_ext) {
        for(long long i=0; i<ref_frames; i++) {
            for (int c = 0; c < ref_chans; c++) {
                wd->ref_samples[c][i] = ref_ext[(ref_start + i) * ref_chans + c];
            }
        }
        buffer_unlocksamples(ref_obj);
    }

    float *subj_ext = buffer_locksamples(subj_obj);
    if (subj_ext) {
        for(long long i=0; i<subj_frames; i++) {
            for (int c = 0; c < subj_chans; c++) {
                wd->subj_samples[c][i] = subj_ext[(subj_start + i) * subj_chans + c];
            }
        }
        buffer_unlocksamples(subj_obj);
    }

    // Resize and clear destination buffer before background alignment
    t_buffer_ref *dest_ref = buffer_ref_new((t_object *)x, dest_name);
    t_buffer_obj *dest_obj = buffer_ref_getobject(dest_ref);
    if (dest_obj) {
        if (buffer_getframecount(dest_obj) != full_ref_frames) {
            int dest_chans = (int)buffer_getchannelcount(dest_obj);
            t_atom av;
            atom_setlong(&av, (t_atom_long)full_ref_frames);
            object_method_typed(dest_obj, gensym("sizeinsamps"), 1, &av, NULL);

            float *dest_ext = buffer_locksamples(dest_obj);
            if (dest_ext) {
                memset(dest_ext, 0, full_ref_frames * dest_chans * sizeof(float));
                buffer_unlocksamples(dest_obj);
                buffer_setdirty(dest_obj);
            }
        }
    }
    object_free(dest_ref);

    wd->ref_frames = ref_frames;
    wd->ref_chans = ref_chans;
    wd->subj_frames = subj_frames;
    wd->subj_chans = subj_chans;
    wd->ref_sr = ref_sr;
    wd->subj_sr = subj_sr;
    wd->dest_buf_name = dest_name;
    wd->targetbias = x->targetbias;
    wd->full_ref_frames = full_ref_frames;
    wd->start_frame = ref_start;

    object_free(ref_ref);
    object_free(subj_ref);

    x->is_busy = 1;
    x->progress = 0.0;
    x->last_error[0] = '\0';
    x->current_wd = wd;
    x->last_dest_name = dest_name;

    systhread_create((method)doubles_worker_thread, x, 0, 0, 0, &x->thread);
    critical_exit(x->lock);
    return;

align_mem_err:
    if (wd->ref_samples) {
        for (int c = 0; c < ref_chans; c++) if (wd->ref_samples[c]) sysmem_freeptr(wd->ref_samples[c]);
        sysmem_freeptr(wd->ref_samples);
    }
    if (wd->subj_samples) {
        for (int c = 0; c < subj_chans; c++) if (wd->subj_samples[c]) sysmem_freeptr(wd->subj_samples[c]);
        sysmem_freeptr(wd->subj_samples);
    }
    sysmem_freeptr(wd);
    object_free(ref_ref);
    object_free(subj_ref);

    x->is_busy = 0;
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

    if (ref_mfccs) for (int i = 0; i < ref_mfcc_len; i++) ref_mfccs[i] = NULL;
    if (subj_mfccs) for (int i = 0; i < subj_mfcc_len; i++) subj_mfccs[i] = NULL;

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
        if (!ref_mfccs[i]) {
            critical_enter(x->lock);
            snprintf(x->last_error, 256, "Memory allocation failed for ref_mfccs[%d]", i);
            x->progress = 1.0;
            critical_exit(x->lock);
            goto cleanup;
        }
        double *segment = (double *)malloc(win_size * sizeof(double));
        if (segment) {
            // MFCC uses first channel for analysis
            for(int j=0; j<win_size; j++) segment[j] = wd->ref_samples[0][i * hop_size + j];
            calculate_mfcc(segment, win_size, fft_size, mfb_ref, num_ceps, ref_mfccs[i]);
            free(segment);
        } else {
            critical_enter(x->lock);
            snprintf(x->last_error, 256, "Memory allocation failed for ref segment %d", i);
            x->progress = 1.0;
            critical_exit(x->lock);
            goto cleanup;
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
        if (!subj_mfccs[i]) {
            critical_enter(x->lock);
            snprintf(x->last_error, 256, "Memory allocation failed for subj_mfccs[%d]", i);
            x->progress = 1.0;
            critical_exit(x->lock);
            goto cleanup;
        }
        double *segment = (double *)malloc(win_size * sizeof(double));
        if (segment) {
            // MFCC uses first channel for analysis
            for(int j=0; j<win_size; j++) segment[j] = wd->subj_samples[0][i * hop_size + j];
            calculate_mfcc(segment, win_size, fft_size, mfb_subj, num_ceps, subj_mfccs[i]);
            free(segment);
        } else {
            critical_enter(x->lock);
            snprintf(x->last_error, 256, "Memory allocation failed for subj segment %d", i);
            x->progress = 1.0;
            critical_exit(x->lock);
            goto cleanup;
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
        detect_transients(wd->ref_samples[0], wd->ref_frames, win_size, hop_size, ref_transients);
        detect_transients(wd->subj_samples[0], wd->subj_frames, win_size, hop_size, subj_transients);
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

        float **dest_samples = (float **)sysmem_newptrclear(wd->subj_chans * sizeof(float *));
        bool alloc_ok = (dest_samples != NULL);
        if (alloc_ok) {
            for (int c = 0; c < wd->subj_chans; c++) {
                dest_samples[c] = (float *)sysmem_newptrclear(wd->ref_frames * sizeof(float));
                if (!dest_samples[c]) {
                    alloc_ok = false;
                    break;
                }
            }
        }

        if (alloc_ok) {
            wsola_process(wd->ref_samples, wd->ref_chans, wd->ref_frames, wd->subj_samples, wd->subj_chans, wd->subj_frames, dest_samples, path, hop_size, win_size, wd->targetbias);

            critical_enter(x->lock);
            x->progress = 0.9;
            x->defer_samples = dest_samples;
            x->defer_chans = wd->subj_chans;
            x->defer_new_frames = wd->ref_frames;
            x->defer_full_ref_frames = wd->full_ref_frames;
            x->defer_start_frame = wd->start_frame;
            x->defer_dest_name = wd->dest_buf_name;

            // Identify inflection points in the DTW path for the function object
            // Inflection points: start, end, and whenever the step type changes
            t_doubles_func_point *fpoints = (t_doubles_func_point *)malloc(path->length * sizeof(t_doubles_func_point));
            int fp_count = 0;
            if (fpoints) {
                for (int i = 0; i < path->length; i++) {
                    bool is_inflection = false;
                    if (i == 0 || i == path->length - 1) {
                        is_inflection = true;
                    } else {
                        // Check if slope changed
                        int dx1 = path->points[i].ref_idx - path->points[i-1].ref_idx;
                        int dy1 = path->points[i].subj_idx - path->points[i-1].subj_idx;
                        int dx2 = path->points[i+1].ref_idx - path->points[i].ref_idx;
                        int dy2 = path->points[i+1].subj_idx - path->points[i].subj_idx;
                        if (dx1 != dx2 || dy1 != dy2) {
                            is_inflection = true;
                        }
                    }

                    if (is_inflection) {
                        fpoints[fp_count].x = (double)path->points[i].ref_idx / (ref_mfcc_len - 1);
                        fpoints[fp_count].y = (double)path->points[i].subj_idx / (subj_mfcc_len - 1);
                        fp_count++;
                    }
                }
                x->defer_func_points = fpoints;
                x->defer_func_points_count = fp_count;
            }
            critical_exit(x->lock);
        } else {
            critical_enter(x->lock);
            snprintf(x->last_error, 256, "Synthesis memory allocation failed");
            x->progress = 1.0;
            critical_exit(x->lock);

            // Cleanup partial allocations
            if (dest_samples) {
                for (int c = 0; c < wd->subj_chans; c++) {
                    if (dest_samples[c]) sysmem_freeptr(dest_samples[c]);
                }
                sysmem_freeptr(dest_samples);
            }
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
        if (wd->ref_samples) {
            for (int c = 0; c < wd->ref_chans; c++) if (wd->ref_samples[c]) sysmem_freeptr(wd->ref_samples[c]);
            sysmem_freeptr(wd->ref_samples);
        }
        if (wd->subj_samples) {
            for (int c = 0; c < wd->subj_chans; c++) if (wd->subj_samples[c]) sysmem_freeptr(wd->subj_samples[c]);
            sysmem_freeptr(wd->subj_samples);
        }
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
    float **samples = x->defer_samples;
    int chans = x->defer_chans;
    long long frames = x->defer_new_frames;
    long long full_ref_frames = x->defer_full_ref_frames;
    long long start_frame = x->defer_start_frame;
    t_symbol *dest_name = x->defer_dest_name;
    t_doubles_func_point *fpoints = x->defer_func_points;
    int fp_count = x->defer_func_points_count;
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
            if (buffer_getframecount(dest_obj) != full_ref_frames) {
                t_atom av;
                atom_setlong(&av, (t_atom_long)full_ref_frames); // sizeinsamps message for buffer~ is per-channel
                object_method_typed(dest_obj, gensym("sizeinsamps"), 1, &av, NULL);
            }
            int dest_chans = (int)buffer_getchannelcount(dest_obj);
            float *dest_ext = buffer_locksamples(dest_obj);
            if (dest_ext) {
                for(long long i=0; i<frames; i++) {
                    long long dest_idx = start_frame + i;
                    if (dest_idx >= 0 && dest_idx < full_ref_frames) {
                        for(int c=0; c<dest_chans; c++) {
                            // Use corresponding subject channel if available, otherwise fallback to first channel
                            int src_c = (c < chans) ? c : 0;
                            dest_ext[dest_idx * dest_chans + c] = samples[src_c][i];
                        }
                    }
                }
                buffer_unlocksamples(dest_obj);
                buffer_setdirty(dest_obj);
            }
        }
        object_free(dest_ref);

        critical_enter(x->lock);
        for (int i = 0; i < x->defer_chans; i++) {
            if (x->defer_samples[i]) sysmem_freeptr(x->defer_samples[i]);
        }
        sysmem_freeptr(x->defer_samples);
        x->defer_samples = NULL;
        x->defer_chans = 0;
        critical_exit(x->lock);
    }

    if (fpoints) {
        outlet_anything(x->function_outlet, gensym("clear"), 0, NULL);
        for (int i = 0; i < fp_count; i++) {
            t_atom av[2];
            atom_setfloat(&av[0], (float)fpoints[i].x);
            atom_setfloat(&av[1], (float)fpoints[i].y);
            outlet_list(x->function_outlet, NULL, 2, av);
        }

        critical_enter(x->lock);
        free(x->defer_func_points);
        x->defer_func_points = NULL;
        x->defer_func_points_count = 0;
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
        sprintf(s, "Inlet 1 (anything): 'align [ref] [subj] [dest]' or 'export [project_path]'");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (float/bang): Progress (0.0-1.0), then 'finished [dest]' message and bang"); break;
            case 1: sprintf(s, "Outlet 2 (list): Inflection points for 'function' object (clear, then x y pairs)"); break;
            case 2: sprintf(s, "Outlet 3 (anything): Logging Outlet"); break;
        }
    }
}

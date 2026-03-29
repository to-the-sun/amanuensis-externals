#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>

typedef struct _buffer_storage {
    float *samples;
    long long frame_count;
    long chans;
    double samplerate;
} t_buffer_storage;

typedef struct _bar_result {
    long has_audio;
} t_bar_result;

typedef struct _buffer_result {
    t_bar_result *bars;
    long long num_bars;
    long long bar_frames;
    char log_msg[256];
} t_buffer_result;

typedef struct _cropsilence_worker_data {
    double bar_ms;
    t_buffer_result *results;
    long num_buffers;
} t_cropsilence_worker_data;

typedef struct _cropsilence {
    t_object b_obj;
    t_symbol *poly_name;
    long log;
    void *log_outlet;
    void *status_outlet;
    t_buffer_storage *ground_truth;
    long num_ground_truth;
    t_critical lock;
    t_qelem *qelem;
    t_systhread thread;
    long is_busy;
    t_cropsilence_worker_data *current_worker_data;
} t_cropsilence;

void *cropsilence_new(t_symbol *s, long argc, t_atom *argv);
void cropsilence_free(t_cropsilence *x);
void cropsilence_int(t_cropsilence *x, t_atom_long n);
void cropsilence_float(t_cropsilence *x, double f);
void cropsilence_assist(t_cropsilence *x, void *b, long m, long a, char *s);

void cropsilence_log(t_cropsilence *x, const char *fmt, ...);
void cropsilence_error(t_cropsilence *x, const char *fmt, ...);
void cropsilence_clear_ground_truth(t_cropsilence *x);
void cropsilence_bind(t_cropsilence *x, t_symbol *s);
void cropsilence_execute(t_cropsilence *x, double bar_ms);
void cropsilence_worker_thread(t_cropsilence *x);
void cropsilence_qfn(t_cropsilence *x);

static t_class *cropsilence_class;

void ext_main(void *r) {
    common_symbols_init();

    t_class *c = class_new("cropsilence~", (method)cropsilence_new, (method)cropsilence_free, sizeof(t_cropsilence), 0L, A_GIMME, 0);

    class_addmethod(c, (method)cropsilence_int, "int", A_LONG, 0);
    class_addmethod(c, (method)cropsilence_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)cropsilence_bind, "bind", A_SYM, 0);
    class_addmethod(c, (method)cropsilence_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_cropsilence, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    cropsilence_class = c;
}

void cropsilence_log(t_cropsilence *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "cropsilence~", fmt, args);
    va_end(args);
}

void cropsilence_error(t_cropsilence *x, const char *fmt, ...) {
    va_list args;
    char buf[4096];
    va_start(args, fmt);
    vsnprintf(buf, 4096, fmt, args);
    object_error((t_object *)x, "%s", buf);
    va_end(args);
}

void *cropsilence_new(t_symbol *s, long argc, t_atom *argv) {
    t_cropsilence *x = (t_cropsilence *)object_alloc(cropsilence_class);

    if (x) {
        x->poly_name = _sym_nothing;
        x->log = 0;
        x->ground_truth = NULL;
        x->num_ground_truth = 0;
        critical_new(&x->lock);
        x->qelem = qelem_new((t_object *)x, (method)cropsilence_qfn);
        x->thread = NULL;
        x->is_busy = 0;
        x->current_worker_data = NULL;

        attr_args_process(x, argc, argv);

        // Create outlets from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);
    }
    return x;
}

void cropsilence_free(t_cropsilence *x) {
    if (x->thread) {
        unsigned int status;
        systhread_join(x->thread, &status);
        x->thread = NULL;
    }
    if (x->qelem) {
        qelem_free(x->qelem);
        x->qelem = NULL;
    }
    if (x->current_worker_data) {
        t_cropsilence_worker_data *wd = x->current_worker_data;
        if (wd->results) {
            for (long i = 0; i < wd->num_buffers; i++) {
                if (wd->results[i].bars) {
                    sysmem_freeptr(wd->results[i].bars);
                }
            }
            sysmem_freeptr(wd->results);
        }
        sysmem_freeptr(wd);
        x->current_worker_data = NULL;
    }
    cropsilence_clear_ground_truth(x);
    critical_free(x->lock);
}

void cropsilence_clear_ground_truth(t_cropsilence *x) {
    if (x->ground_truth) {
        for (long i = 0; i < x->num_ground_truth; i++) {
            if (x->ground_truth[i].samples) {
                sysmem_freeptr(x->ground_truth[i].samples);
            }
        }
        sysmem_freeptr(x->ground_truth);
        x->ground_truth = NULL;
    }
    x->num_ground_truth = 0;
}

void cropsilence_bind(t_cropsilence *x, t_symbol *s) {
    critical_enter(x->lock);
    if (x->is_busy) {
        cropsilence_error(x, "Cannot bind while a cropping operation is in progress.");
        critical_exit(x->lock);
        return;
    }
    if (s == x->poly_name && x->ground_truth != NULL) {
        cropsilence_log(x, "BIND: Already bound to '%s'. Skipping ground truth copy.", s->s_name);
        critical_exit(x->lock);
        return;
    }

    cropsilence_log(x, "BIND: Binding to polybuffer~ '%s' and creating ground truth copy...", s->s_name);
    cropsilence_clear_ground_truth(x);
    x->poly_name = s;

    // Count buffers
    long count = 0;
    char bufname[256];
    t_buffer_ref *b_ref = NULL;

    while (true) {
        snprintf(bufname, 256, "%s.%ld", x->poly_name->s_name, count + 1);
        if (b_ref == NULL) {
            b_ref = buffer_ref_new((t_object *)x, gensym(bufname));
        } else {
            buffer_ref_set(b_ref, gensym(bufname));
        }

        if (buffer_ref_getobject(b_ref)) {
            count++;
        } else {
            break;
        }
    }

    if (count > 0) {
        x->ground_truth = (t_buffer_storage *)sysmem_newptrclear(count * sizeof(t_buffer_storage));
        if (x->ground_truth) {
            x->num_ground_truth = count;
            for (long i = 0; i < count; i++) {
                snprintf(bufname, 256, "%s.%ld", x->poly_name->s_name, i + 1);
                buffer_ref_set(b_ref, gensym(bufname));
                t_buffer_obj *b = buffer_ref_getobject(b_ref);

                x->ground_truth[i].frame_count = buffer_getframecount(b);
                x->ground_truth[i].chans = buffer_getchannelcount(b);
                x->ground_truth[i].samplerate = buffer_getsamplerate(b);

                cropsilence_log(x, "STORING: %s (%lld frames, %ld chans, %.2f Hz)",
                    bufname, x->ground_truth[i].frame_count, x->ground_truth[i].chans, x->ground_truth[i].samplerate);

                long long total_samples = x->ground_truth[i].frame_count * x->ground_truth[i].chans;
                if (total_samples > 0) {
                    x->ground_truth[i].samples = (float *)sysmem_newptr(total_samples * sizeof(float));
                    if (x->ground_truth[i].samples) {
                        float *ext_samples = buffer_locksamples(b);
                        if (ext_samples) {
                            memcpy(x->ground_truth[i].samples, ext_samples, total_samples * sizeof(float));
                            buffer_unlocksamples(b);
                        } else {
                            cropsilence_error(x, "Could not lock samples for %s", bufname);
                        }
                    } else {
                        cropsilence_error(x, "Memory allocation failed for ground truth of %s", bufname);
                    }
                }
            }
            cropsilence_log(x, "BIND SUCCESS: Stored ground truth for %ld buffers.", count);
        } else {
            cropsilence_error(x, "Memory allocation failed for ground truth array.");
            x->num_ground_truth = 0;
        }
    } else {
        cropsilence_error(x, "No buffers found for polybuffer~ '%s'", s->s_name);
    }

    if (b_ref) {
        object_free(b_ref);
    }
    critical_exit(x->lock);
}

void cropsilence_int(t_cropsilence *x, t_atom_long n) {
    cropsilence_execute(x, (double)n);
}

void cropsilence_float(t_cropsilence *x, double f) {
    cropsilence_execute(x, f);
}

void cropsilence_execute(t_cropsilence *x, double bar_ms) {
    critical_enter(x->lock);
    if (x->is_busy) {
        cropsilence_error(x, "Object is busy with a previous cropping operation.");
        critical_exit(x->lock);
        return;
    }

    if (bar_ms <= 0) {
        cropsilence_error(x, "Invalid bar length: %f", bar_ms);
        critical_exit(x->lock);
        return;
    }

    if (x->poly_name == _sym_nothing || x->ground_truth == NULL) {
        cropsilence_error(x, "No polybuffer~ bound or ground truth missing");
        critical_exit(x->lock);
        return;
    }

    x->is_busy = 1;
    t_cropsilence_worker_data *wd = (t_cropsilence_worker_data *)sysmem_newptrclear(sizeof(t_cropsilence_worker_data));
    if (wd) {
        wd->bar_ms = bar_ms;
        wd->num_buffers = x->num_ground_truth;
        wd->results = (t_buffer_result *)sysmem_newptrclear(wd->num_buffers * sizeof(t_buffer_result));

        if (wd->results) {
            x->current_worker_data = wd;
            cropsilence_log(x, "START ASYNC: Cropping polybuffer~ '%s' with bar length %.2f ms", x->poly_name->s_name, bar_ms);
            systhread_create((method)cropsilence_worker_thread, x, 0, 0, 0, &x->thread);
        } else {
            cropsilence_error(x, "Memory allocation failed for worker results.");
            sysmem_freeptr(wd);
            x->is_busy = 0;
        }
    } else {
        cropsilence_error(x, "Memory allocation failed for worker data.");
        x->is_busy = 0;
    }
    critical_exit(x->lock);
}

void cropsilence_worker_thread(t_cropsilence *x) {
    t_cropsilence_worker_data *wd = x->current_worker_data;
    if (!wd) {
        systhread_exit(0);
        return;
    }

    // Since we check is_busy in bind() and execute(), and join the thread in free(),
    // ground_truth is guaranteed to be stable for the duration of this thread.
    // We don't need to hold the lock for the entire scan.
    for (long i = 0; i < wd->num_buffers; i++) {
        t_buffer_storage *gt = &x->ground_truth[i];
        t_buffer_result *res = &wd->results[i];

        double sr = gt->samplerate;
        long long total_frames = gt->frame_count;
        long chans = gt->chans;
        float *samples = gt->samples;

        if (sr > 0 && total_frames > 0 && chans > 0 && samples) {
            long long bar_frames = (long long)ceil((wd->bar_ms * sr) / 1000.0);
            if (bar_frames <= 0) {
                snprintf(res->log_msg, 256, "SKIPPED: %ld (bar length too short)", i + 1);
                continue;
            }

            long long num_bars = (total_frames + bar_frames - 1) / bar_frames;
            res->bars = (t_bar_result *)sysmem_newptrclear(num_bars * sizeof(t_bar_result));
            res->num_bars = num_bars;
            res->bar_frames = bar_frames;

            if (res->bars) {
                long long kept_count = 0;
                for (long long b_idx = 0; b_idx < num_bars; b_idx++) {
                    long long start_frame = b_idx * bar_frames;
                    long long end_frame = start_frame + bar_frames;
                    if (end_frame > total_frames) end_frame = total_frames;

                    int has_audio = 0;
                    for (long long f = start_frame; f < end_frame; f++) {
                        for (long c = 0; c < chans; c++) {
                            if (fabs(samples[f * chans + c]) > 0.00009) {
                                has_audio = 1;
                                break;
                            }
                        }
                        if (has_audio) break;
                    }
                    if (has_audio) {
                        res->bars[b_idx].has_audio = 1;
                        kept_count++;
                    }
                }
                snprintf(res->log_msg, 256, "Buffer %ld: Kept %lld/%lld bars.", i + 1, kept_count, num_bars);
            } else {
                snprintf(res->log_msg, 256, "ERROR: Memory allocation failed for buffer %ld scan results.", i + 1);
            }
        } else {
            snprintf(res->log_msg, 256, "SKIPPED: %ld (invalid buffer or no samples in ground truth)", i + 1);
        }
    }
    qelem_set(x->qelem);
    systhread_exit(0);
}

void cropsilence_qfn(t_cropsilence *x) {
    critical_enter(x->lock);
    t_cropsilence_worker_data *wd = x->current_worker_data;

    if (!wd) {
        x->is_busy = 0;
        critical_exit(x->lock);
        return;
    }

    char bufname[256];
    t_buffer_ref *b_ref = NULL;

    for (long i = 0; i < wd->num_buffers; i++) {
        t_buffer_result *res = &wd->results[i];
        if (res->log_msg[0]) {
            cropsilence_log(x, "%s", res->log_msg);
        }

        if (res->bars) {
            snprintf(bufname, 256, "%s.%ld", x->poly_name->s_name, i + 1);
            t_symbol *s_member = gensym(bufname);

            if (b_ref == NULL) {
                b_ref = buffer_ref_new((t_object *)x, s_member);
            } else {
                buffer_ref_set(b_ref, s_member);
            }

            t_buffer_obj *b = buffer_ref_getobject(b_ref);
            if (!b) {
                cropsilence_log(x, "SKIPPED APPLY: %s (external buffer not found)", bufname);
                continue;
            }

            t_buffer_storage *gt = &x->ground_truth[i];
            long chans = gt->chans;
            float *samples = gt->samples;
            long long total_frames = gt->frame_count;

            long long kept_count = 0;
            for (long long b_idx = 0; b_idx < res->num_bars; b_idx++) {
                if (res->bars[b_idx].has_audio) kept_count++;
            }

            long long new_total_frames = kept_count * res->bar_frames;

            // Resize external buffer
            t_atom av;
            atom_setlong(&av, (t_atom_long)(new_total_frames * chans));
            object_method_typed(b, gensym("sizeinsamps"), 1, &av, NULL);

            if (new_total_frames > 0) {
                float *new_samples = (float *)sysmem_newptrclear(new_total_frames * chans * sizeof(float));
                if (new_samples) {
                    long long current_kept_bar = 0;
                    for (long long b_idx = 0; b_idx < res->num_bars; b_idx++) {
                        if (res->bars[b_idx].has_audio) {
                            long long src_start = b_idx * res->bar_frames;
                            long long dst_start = current_kept_bar * res->bar_frames;
                            long long frames_to_copy = res->bar_frames;
                            if (src_start + frames_to_copy > total_frames) {
                                frames_to_copy = total_frames - src_start;
                            }
                            memcpy(new_samples + (dst_start * chans), samples + (src_start * chans), frames_to_copy * chans * sizeof(float));
                            current_kept_bar++;
                        }
                    }

                    buffer_edit_begin(b);
                    float *res_samples = buffer_locksamples(b);
                    if (res_samples) {
                        long new_chans = buffer_getchannelcount(b);
                        if (new_chans == chans) {
                            memcpy(res_samples, new_samples, new_total_frames * chans * sizeof(float));
                        } else {
                            cropsilence_error(x, "Channel count mismatch for %s after resize (expected %ld, got %ld)", bufname, chans, new_chans);
                        }
                        buffer_unlocksamples(b);
                    } else {
                        cropsilence_error(x, "Could not lock external samples for %s", bufname);
                    }
                    buffer_edit_end(b, 1);
                    buffer_setdirty(b);
                    sysmem_freeptr(new_samples);
                } else {
                    cropsilence_error(x, "Memory allocation failed for cropping %s", bufname);
                }
            } else {
                buffer_edit_begin(b);
                buffer_edit_end(b, 1);
                buffer_setdirty(b);
            }
        }
    }

    if (b_ref) {
        object_free(b_ref);
    }

    // Clean up worker data
    if (wd->results) {
        for (long i = 0; i < wd->num_buffers; i++) {
            if (wd->results[i].bars) {
                sysmem_freeptr(wd->results[i].bars);
            }
        }
        sysmem_freeptr(wd->results);
    }
    sysmem_freeptr(wd);
    x->current_worker_data = NULL;

    // Join thread safely (it should already be exited or about to exit)
    if (x->thread) {
        unsigned int status;
        systhread_join(x->thread, &status);
        x->thread = NULL;
    }

    x->is_busy = 0;
    cropsilence_log(x, "END ASYNC: Finished cropping polybuffer~ '%s'", x->poly_name->s_name);
    outlet_bang(x->status_outlet);
    critical_exit(x->lock);
}

void cropsilence_assist(t_cropsilence *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1 (float/int/symbol): Trigger cropping with bar length (ms) or Bind to polybuffer~");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (bang): Bang when asynchronous cropping is complete"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Detailed Diagnostic Logging Outlet"); break;
        }
    }
}

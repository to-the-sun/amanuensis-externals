#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>

typedef struct _buffer_storage {
    float *samples;
    long long frame_count;
    long chans;
    double samplerate;
} t_buffer_storage;

typedef struct _cropsilence {
    t_object b_obj;
    t_symbol *poly_name;
    long log;
    void *log_outlet;
    void *status_outlet;
    t_buffer_storage *ground_truth;
    long num_ground_truth;
    t_critical lock;
} t_cropsilence;

void *cropsilence_new(t_symbol *s, long argc, t_atom *argv);
void cropsilence_free(t_cropsilence *x);
void cropsilence_int(t_cropsilence *x, t_atom_long n);
void cropsilence_float(t_cropsilence *x, double f);
void cropsilence_assist(t_cropsilence *x, void *b, long m, long a, char *s);

void cropsilence_log(t_cropsilence *x, const char *fmt, ...);
void cropsilence_clear_ground_truth(t_cropsilence *x);
void cropsilence_bind(t_cropsilence *x, t_symbol *s);
void cropsilence_execute(t_cropsilence *x, double bar_ms);

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

void *cropsilence_new(t_symbol *s, long argc, t_atom *argv) {
    t_cropsilence *x = (t_cropsilence *)object_alloc(cropsilence_class);

    if (x) {
        x->poly_name = _sym_nothing;
        x->log = 0;
        x->ground_truth = NULL;
        x->num_ground_truth = 0;
        critical_new(&x->lock);

        attr_args_process(x, argc, argv);

        // Create outlets from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);
    }
    return x;
}

void cropsilence_free(t_cropsilence *x) {
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

                long long total_samples = x->ground_truth[i].frame_count * x->ground_truth[i].chans;
                if (total_samples > 0) {
                    x->ground_truth[i].samples = (float *)sysmem_newptr(total_samples * sizeof(float));
                    if (x->ground_truth[i].samples) {
                        float *ext_samples = buffer_locksamples(b);
                        if (ext_samples) {
                            memcpy(x->ground_truth[i].samples, ext_samples, total_samples * sizeof(float));
                            buffer_unlocksamples(b);
                        } else {
                            cropsilence_log(x, "ERROR: Could not lock samples for %s", bufname);
                        }
                    } else {
                        cropsilence_log(x, "ERROR: Memory allocation failed for ground truth of %s", bufname);
                    }
                }
            }
            cropsilence_log(x, "BIND SUCCESS: Stored ground truth for %ld buffers.", count);
        } else {
            cropsilence_log(x, "ERROR: Memory allocation failed for ground truth array.");
            x->num_ground_truth = 0;
        }
    } else {
        cropsilence_log(x, "BIND ERROR: No buffers found for polybuffer~ '%s'", s->s_name);
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
    if (bar_ms <= 0) {
        object_error((t_object *)x, "invalid bar length: %f", bar_ms);
        critical_exit(x->lock);
        return;
    }

    if (x->poly_name == _sym_nothing || x->ground_truth == NULL) {
        object_error((t_object *)x, "no polybuffer~ bound or ground truth missing");
        critical_exit(x->lock);
        return;
    }

    cropsilence_log(x, "START: Cropping polybuffer~ '%s' using ground truth as source with bar length %.2f ms", x->poly_name->s_name, bar_ms);

    char bufname[256];
    t_buffer_ref *b_ref = NULL;

    for (long i = 0; i < x->num_ground_truth; i++) {
        t_buffer_storage *gt = &x->ground_truth[i];
        snprintf(bufname, 256, "%s.%ld", x->poly_name->s_name, i + 1);
        t_symbol *s_member = gensym(bufname);

        if (b_ref == NULL) {
            b_ref = buffer_ref_new((t_object *)x, s_member);
        } else {
            buffer_ref_set(b_ref, s_member);
        }

        t_buffer_obj *b = buffer_ref_getobject(b_ref);
        if (!b) {
            cropsilence_log(x, "SKIPPED: %s (external buffer not found)", bufname);
            continue;
        }

        double sr = gt->samplerate;
        long long total_frames = gt->frame_count;
        long chans = gt->chans;
        float *samples = gt->samples;

        if (sr > 0 && total_frames > 0 && chans > 0 && samples) {
            long long bar_frames = (long long)ceil((bar_ms * sr) / 1000.0);
            if (bar_frames <= 0) {
                cropsilence_log(x, "SKIPPED: %s (bar length too short for sample rate)", bufname);
                continue;
            }

            long long num_bars = (total_frames + bar_frames - 1) / bar_frames;
            int *keep_bar = (int *)sysmem_newptr(num_bars * sizeof(int));
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
                    keep_bar[b_idx] = 1;
                    kept_count++;
                } else {
                    keep_bar[b_idx] = 0;
                }
            }

            // Always apply result from ground truth, even if kept_count == num_bars (this resets any previous cropping)
            cropsilence_log(x, "%s: Kept %lld bars out of %lld. Applying to external buffer.", bufname, kept_count, num_bars);

            long long new_total_frames = kept_count * bar_frames;
            float *new_samples = (float *)sysmem_newptrclear(new_total_frames * chans * sizeof(float));
            if (new_samples) {
                long long current_kept_bar = 0;
                for (long long b_idx = 0; b_idx < num_bars; b_idx++) {
                    if (keep_bar[b_idx]) {
                        long long src_start = b_idx * bar_frames;
                        long long dst_start = current_kept_bar * bar_frames;
                        long long frames_to_copy = bar_frames;
                        if (src_start + frames_to_copy > total_frames) {
                            frames_to_copy = total_frames - src_start;
                        }
                        memcpy(new_samples + (dst_start * chans), samples + (src_start * chans), frames_to_copy * chans * sizeof(float));
                        current_kept_bar++;
                    }
                }

                // Resize external buffer
                buffer_edit_begin(b);
                t_atom av;
                atom_setlong(&av, (t_atom_long)new_total_frames);
                object_method_typed(b, gensym("sizeinsamps"), 1, &av, NULL);

                float *res_samples = buffer_locksamples(b);
                if (res_samples) {
                    long new_chans = buffer_getchannelcount(b);
                    if (new_chans == chans) {
                        memcpy(res_samples, new_samples, new_total_frames * chans * sizeof(float));
                    } else {
                        cropsilence_log(x, "ERROR: Channel count mismatch for %s after resize", bufname);
                    }
                    buffer_unlocksamples(b);
                } else {
                    cropsilence_log(x, "ERROR: Could not lock external samples for %s", bufname);
                }
                buffer_edit_end(b, 1);
                buffer_setdirty(b);
                sysmem_freeptr(new_samples);
            } else {
                cropsilence_log(x, "ERROR: Memory allocation failed for cropping %s", bufname);
            }
            sysmem_freeptr(keep_bar);
        } else {
            cropsilence_log(x, "SKIPPED: %s (invalid buffer parameters or samples missing in ground truth)", bufname);
        }
    }

    if (b_ref) {
        object_free(b_ref);
    }
    cropsilence_log(x, "END: Finished cropping polybuffer~ '%s'", x->poly_name->s_name);
    outlet_bang(x->status_outlet);
    critical_exit(x->lock);
}

void cropsilence_assist(t_cropsilence *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (number) Trigger cropping with specified bar length (ms); (bind symbol) Bind to polybuffer~");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (bang): Bang when done"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Logging Outlet"); break;
        }
    }
}

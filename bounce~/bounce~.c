#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include "../shared/crossfade.h"
#include <string.h>
#include <math.h>

typedef struct _bounce {
    t_object b_obj;
    t_symbol *poly_prefix;
    t_symbol *dest_name;
    t_buffer_ref *poly_ref; // Canary for polybuffer (prefix.1)
    t_buffer_ref *dest_ref;
    long log;
    void *log_outlet;
    void *bang_outlet;
    long poly_found;
    long dest_found;
    long poly_error_sent;
    long dest_error_sent;
    double normalize_to;
    double low_ms;
    double high_ms;
} t_bounce;

void *bounce_new(t_symbol *s, long argc, t_atom *argv);
void bounce_free(t_bounce *x);
void bounce_assist(t_bounce *x, void *b, long m, long a, char *s);
void bounce_bang(t_bounce *x);
void bounce_check_attachments(t_bounce *x, long report_error);
void bounce_log(t_bounce *x, const char *fmt, ...);

static t_class *bounce_class;

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("bounce~", (method)bounce_new, (method)bounce_free, sizeof(t_bounce), 0L, A_GIMME, 0);

    class_addmethod(c, (method)bounce_bang, "bang", 0);
    class_addmethod(c, (method)bounce_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_bounce, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_DOUBLE(c, "normalize", 0, t_bounce, normalize_to);
    CLASS_ATTR_LABEL(c, "normalize", 0, "Normalization Target Amplitude");
    CLASS_ATTR_DEFAULT(c, "normalize", 0, "0");

    CLASS_ATTR_DOUBLE(c, "low", 0, t_bounce, low_ms);
    CLASS_ATTR_LABEL(c, "low", 0, "Low Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "low", 0, "22.653");

    CLASS_ATTR_DOUBLE(c, "high", 0, t_bounce, high_ms);
    CLASS_ATTR_LABEL(c, "high", 0, "High Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "high", 0, "4999.0");

    class_register(CLASS_BOX, c);
    bounce_class = c;
}

void bounce_log(t_bounce *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "bounce~", fmt, args);
    va_end(args);
}

void bounce_check_attachments(t_bounce *x, long report_error) {
    // Polybuffer check (via prefix.1)
    if (x->poly_prefix != _sym_nothing) {
        t_buffer_obj *b = buffer_ref_getobject(x->poly_ref);
        if (!b) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            buffer_ref_set(x->poly_ref, _sym_nothing);
            buffer_ref_set(x->poly_ref, gensym(t1name));
            b = buffer_ref_getobject(x->poly_ref);
        }
        if (b) {
            x->poly_error_sent = 0; // Reset flag when buffer is successfully found
            if (!x->poly_found) {
                bounce_log(x, "successfully found polybuffer~ '%s' (via %s.1)", x->poly_prefix->s_name, x->poly_prefix->s_name);
                x->poly_found = 1;
            }
        } else {
            if (x->poly_found) x->poly_found = 0;
            if (report_error && !x->poly_error_sent) {
                object_error((t_object *)x, "polybuffer~ '%s' not found (could not find %s.1)", x->poly_prefix->s_name, x->poly_prefix->s_name);
                x->poly_error_sent = 1;
            }
        }
    }

    // Destination check
    if (x->dest_name != _sym_nothing) {
        t_buffer_obj *b = buffer_ref_getobject(x->dest_ref);
        if (!b) {
            buffer_ref_set(x->dest_ref, _sym_nothing);
            buffer_ref_set(x->dest_ref, x->dest_name);
            b = buffer_ref_getobject(x->dest_ref);
        }
        if (b) {
            x->dest_error_sent = 0; // Reset flag when buffer is successfully found
            if (!x->dest_found) {
                bounce_log(x, "successfully found destination buffer~ '%s'", x->dest_name->s_name);
                x->dest_found = 1;
            }
        } else {
            if (x->dest_found) x->dest_found = 0;
            if (report_error && !x->dest_error_sent) {
                object_error((t_object *)x, "destination buffer~ '%s' not found", x->dest_name->s_name);
                x->dest_error_sent = 1;
            }
        }
    }
}

void *bounce_new(t_symbol *s, long argc, t_atom *argv) {
    t_bounce *x = (t_bounce *)object_alloc(bounce_class);

    if (x) {
        x->poly_prefix = _sym_nothing;
        x->dest_name = _sym_nothing;
        x->log = 0;
        x->poly_found = 0;
        x->dest_found = 0;
        x->poly_error_sent = 0;
        x->dest_error_sent = 0;
        x->normalize_to = 0.0;
        x->low_ms = 22.653;
        x->high_ms = 4999.0;

        // Arg 1: Polybuffer prefix
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->poly_prefix = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory polybuffer~ name argument");
        }

        // Arg 2: Destination buffer name
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->dest_name = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory destination buffer~ name argument");
        }

        attr_args_process(x, argc, argv);

        // Outlets (Right-to-Left)
        if (x->log) {
            x->log_outlet = outlet_new((t_object *)x, NULL);
        } else {
            x->log_outlet = NULL;
        }
        x->bang_outlet = outlet_new((t_object *)x, NULL);

        // Init buffer refs
        if (x->poly_prefix != _sym_nothing) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            x->poly_ref = buffer_ref_new((t_object *)x, gensym(t1name));
        } else {
            x->poly_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        }
        x->dest_ref = buffer_ref_new((t_object *)x, x->dest_name);

        // Initial search
        bounce_check_attachments(x, 1);
    }
    return x;
}

void bounce_free(t_bounce *x) {
    if (x->poly_ref) object_free(x->poly_ref);
    if (x->dest_ref) object_free(x->dest_ref);
}

void bounce_assist(t_bounce *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (bang) start bounce, (low/high) set fade limits");
    } else {
        if (a == 0) {
            sprintf(s, "Outlet 1: (bang) finished");
        } else {
            sprintf(s, "Outlet 2: (anything) Logging Outlet");
        }
    }
}

void bounce_bang(t_bounce *x) {
    bounce_check_attachments(x, 1);
    if (!x->poly_found || !x->dest_found) return;

    t_buffer_obj *dest_buf = buffer_ref_getobject(x->dest_ref);
    if (!dest_buf) {
        buffer_ref_set(x->dest_ref, _sym_nothing);
        buffer_ref_set(x->dest_ref, x->dest_name);
        dest_buf = buffer_ref_getobject(x->dest_ref);
    }

    if (!dest_buf) {
        object_error((t_object *)x, "destination buffer~ %s missing during bounce", x->dest_name->s_name);
        return;
    }

    long n_frames_dest = buffer_getframecount(dest_buf);
    long n_chans_dest = buffer_getchannelcount(dest_buf);
    double sr_dest = buffer_getsamplerate(dest_buf);
    if (sr_dest <= 0) sr_dest = 44100.0;

    float *samples_dest = NULL;
    for (int retry = 0; retry < 10; retry++) {
        samples_dest = buffer_locksamples(dest_buf);
        if (samples_dest) break;

        // If locking fails, try a kick
        buffer_ref_set(x->dest_ref, _sym_nothing);
        buffer_ref_set(x->dest_ref, x->dest_name);
        dest_buf = buffer_ref_getobject(x->dest_ref);
        if (!dest_buf) break;

        systhread_sleep(1);
    }

    if (!samples_dest) {
        object_error((t_object *)x, "could not lock destination buffer %s", x->dest_name->s_name);
        return;
    }

    critical_enter(0);
    buffer_edit_begin(dest_buf);

    // Clear destination
    memset(samples_dest, 0, n_frames_dest * n_chans_dest * sizeof(float));

    t_buffer_ref *src_ref = buffer_ref_new((t_object *)x, _sym_nothing);
    int stem_idx = 1;
    while (1) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%d", x->poly_prefix->s_name, stem_idx);
        t_symbol *s_bufname = gensym(bufname);
        buffer_ref_set(src_ref, s_bufname);
        t_buffer_obj *src_buf = buffer_ref_getobject(src_ref);
        if (!src_buf) {
            buffer_ref_set(src_ref, _sym_nothing);
            buffer_ref_set(src_ref, s_bufname);
            src_buf = buffer_ref_getobject(src_ref);
        }
        if (!src_buf) break;

        long n_frames_src = buffer_getframecount(src_buf);
        long n_chans_src = buffer_getchannelcount(src_buf);
        double sr_src = buffer_getsamplerate(src_buf);
        if (sr_src <= 0) sr_src = 44100.0;

        float *samples_src = NULL;
        for (int retry = 0; retry < 10; retry++) {
            samples_src = buffer_locksamples(src_buf);
            if (samples_src) break;

            // If locking fails, try a kick
            buffer_ref_set(src_ref, _sym_nothing);
            buffer_ref_set(src_ref, s_bufname);
            src_buf = buffer_ref_getobject(src_ref);
            if (!src_buf) break;

            // Release global lock before sleeping
            critical_exit(0);
            systhread_sleep(1);
            critical_enter(0);

            // Re-verify dest_buf hasn't been lost during sleep
            dest_buf = buffer_ref_getobject(x->dest_ref);
            if (!dest_buf) {
                buffer_ref_set(x->dest_ref, _sym_nothing);
                buffer_ref_set(x->dest_ref, x->dest_name);
                dest_buf = buffer_ref_getobject(x->dest_ref);
            }
            if (!dest_buf) {
                buffer_unlocksamples(src_buf); // src_buf might be valid but we can't proceed
                break;
            }
        }

        if (!samples_src) {
            bounce_log(x, "Error: could not lock stem buffer %s", bufname);
            stem_idx++;
            continue;
        }

        // 1. Find last audio frame by scanning backward
        long last_audio_frame = -1;
        for (long f = n_frames_src - 1; f >= 0; f--) {
            for (long c = 0; c < n_chans_src; c++) {
                if (samples_src[f * n_chans_src + c] != 0.0f) {
                    last_audio_frame = f;
                    break;
                }
            }
            if (last_audio_frame != -1) break;
        }

        if (last_audio_frame == -1) {
            buffer_unlocksamples(src_buf);
            stem_idx++;
            continue;
        }

        // 2. Normalize if needed
        double max_abs = 0.0;
        for (long f = 0; f < n_frames_src; f++) {
            for (long c = 0; c < n_chans_src; c++) {
                double a = fabs((double)samples_src[f * n_chans_src + c]);
                if (a > max_abs) max_abs = a;
            }
        }

        if (max_abs > 1.0) {
            // Destructive normalization of stems
            // We are already inside global critical section
            buffer_edit_begin(src_buf);
            double scale = 0.9999999 / max_abs;
            for (long f = 0; f < n_frames_src; f++) {
                for (long c = 0; c < n_chans_src; c++) {
                    samples_src[f * n_chans_src + c] *= (float)scale;
                }
            }
            bounce_log(x, "normalized buffer '%s', max absolute value was %f", bufname, max_abs);
            buffer_edit_end(src_buf, 1);
            buffer_setdirty(src_buf);
        }

        // 3. Add to destination with fades
        long fade_out_duration_frames = (long)round(x->high_ms * sr_src / 1000.0);
        long fade_out_start_frame = last_audio_frame - fade_out_duration_frames;
        if (fade_out_start_frame < 0) fade_out_start_frame = 0;

        t_ramp_state ramp;
        ramp_init(&ramp, sr_dest, x->high_ms);
        long long elapsed = 0;
        int fade_in_triggered = 0;
        int fade_out_triggered = 0;

        for (long f_dest = 0; f_dest < n_frames_dest; f_dest++) {
            long f_src = (long)round((double)f_dest * sr_src / sr_dest);
            if (f_src >= 0 && f_src <= last_audio_frame) {
                double movement = 0.0;
                if (!fade_in_triggered) {
                    movement = -1.0;
                    fade_in_triggered = 1;
                } else if (!fade_out_triggered && f_src >= fade_out_start_frame) {
                    movement = 1.0;
                    fade_out_triggered = 1;
                }

                double max_abs_src = 0.0;
                for (long c = 0; c < n_chans_src; c++) {
                    double a = fabs((double)samples_src[f_src * n_chans_src + c]);
                    if (a > max_abs_src) max_abs_src = a;
                }

                double fade_factor;
                ramp_process(&ramp, max_abs_src, movement, elapsed, sr_dest, x->low_ms, x->high_ms, &fade_factor);

                samples_dest[f_dest * n_chans_dest] += (float)((double)samples_src[f_src * n_chans_src] * fade_factor);
                elapsed++;
            }
        }

        buffer_unlocksamples(src_buf);
        stem_idx++;
    }

    // 4. Duplicate channel 0 to all other channels
    if (n_chans_dest > 1) {
        for (long f = 0; f < n_frames_dest; f++) {
            float val = samples_dest[f * n_chans_dest];
            for (long c = 1; c < n_chans_dest; c++) {
                samples_dest[f * n_chans_dest + c] = val;
            }
        }
    }

    // 5. Final normalization of product buffer if requested
    if (x->normalize_to != 0.0) {
        double product_max = 0.0;
        for (long i = 0; i < n_frames_dest * n_chans_dest; i++) {
            double a = fabs((double)samples_dest[i]);
            if (a > product_max) product_max = a;
        }

        if (product_max > 0.0) {
            double target = fabs(x->normalize_to);
            double scale = target / product_max;
            for (long i = 0; i < n_frames_dest * n_chans_dest; i++) {
                samples_dest[i] *= (float)scale;
            }
            bounce_log(x, "final product normalized to %.7f (was %.7f)", target, product_max);
        }
    }

    buffer_edit_end(dest_buf, 1);
    buffer_unlocksamples(dest_buf);
    buffer_setdirty(dest_buf);
    critical_exit(0);

    if (src_ref) object_free(src_ref);
    outlet_bang(x->bang_outlet);
}

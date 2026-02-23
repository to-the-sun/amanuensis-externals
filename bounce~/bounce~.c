#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
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
        sprintf(s, "Inlet 1: (bang) start bounce");
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

    critical_enter(0);

    t_buffer_obj *dest_buf = buffer_ref_getobject(x->dest_ref);
    if (!dest_buf) {
        buffer_ref_set(x->dest_ref, _sym_nothing);
        buffer_ref_set(x->dest_ref, x->dest_name);
        dest_buf = buffer_ref_getobject(x->dest_ref);
    }

    if (!dest_buf) {
        critical_exit(0);
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
        critical_exit(0);
        object_error((t_object *)x, "could not lock destination buffer %s", x->dest_name->s_name);
        return;
    }

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

            systhread_sleep(1);
        }

        if (!samples_src) {
            bounce_log(x, "Error: could not lock stem buffer %s", bufname);
            stem_idx++;
            continue;
        }

        // 1. Normalize if needed
        double max_abs = 0.0;
        for (long f = 0; f < n_frames_src; f++) {
            for (long c = 0; c < n_chans_src; c++) {
                double a = fabs((double)samples_src[f * n_chans_src + c]);
                if (a > max_abs) max_abs = a;
            }
        }

        if (max_abs > 1.0) {
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

        // 2. Add to destination (channel 0)
        for (long f_dest = 0; f_dest < n_frames_dest; f_dest++) {
            long f_src = (long)round((double)f_dest * sr_src / sr_dest);
            if (f_src >= 0 && f_src < n_frames_src) {
                samples_dest[f_dest * n_chans_dest] += samples_src[f_src * n_chans_src];
            }
        }

        buffer_unlocksamples(src_buf);
        stem_idx++;
    }

    // 3. Duplicate channel 0 to all other channels
    if (n_chans_dest > 1) {
        for (long f = 0; f < n_frames_dest; f++) {
            float val = samples_dest[f * n_chans_dest];
            for (long c = 1; c < n_chans_dest; c++) {
                samples_dest[f * n_chans_dest + c] = val;
            }
        }
    }

    buffer_edit_end(dest_buf, 1);
    buffer_unlocksamples(dest_buf);
    buffer_setdirty(dest_buf);
    critical_exit(0);

    object_free(src_ref);
    outlet_bang(x->bang_outlet);
}

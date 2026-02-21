#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "../shared/crossfade.h"
#include "../shared/logging.h"

typedef struct _crossfade {
    t_pxobject x_obj;
    t_crossfade_state state;
    double low_ms;
    double high_ms;
    long log;
    void *log_outlet;
} t_crossfade;

void crossfade_log(t_crossfade *x, const char *fmt, ...);
void *crossfade_new(t_symbol *s, long argc, t_atom *argv);
void crossfade_free(t_crossfade *x);
void crossfade_dsp64(t_crossfade *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void crossfade_perform64(t_crossfade *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void crossfade_assist(t_crossfade *x, void *b, long m, long a, char *s);

static t_class *crossfade_class;

// Helper function to send verbose log messages with prefix
void crossfade_log(t_crossfade *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "crossfade~", fmt, args);
    va_end(args);
}

void ext_main(void *r) {
    t_class *c = class_new("crossfade~", (method)crossfade_new, (method)crossfade_free, sizeof(t_crossfade), 0L, A_GIMME, 0);

    class_addmethod(c, (method)crossfade_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)crossfade_assist, "assist", A_CANT, 0);

    CLASS_ATTR_DOUBLE(c, "low", 0, t_crossfade, low_ms);
    CLASS_ATTR_LABEL(c, "low", 0, "Low Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "low", 0, "22.653"); // Approx 999 samples at 44.1kHz

    CLASS_ATTR_DOUBLE(c, "high", 0, t_crossfade, high_ms);
    CLASS_ATTR_LABEL(c, "high", 0, "High Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "high", 0, "4999.0");

    CLASS_ATTR_LONG(c, "log", 0, t_crossfade, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    crossfade_class = c;
}

void *crossfade_new(t_symbol *s, long argc, t_atom *argv) {
    t_crossfade *x = (t_crossfade *)object_alloc(crossfade_class);

    if (x) {
        x->low_ms = 22.653;
        x->high_ms = 4999.0;
        x->log = 0;
        x->log_outlet = NULL;

        dsp_setup((t_pxobject *)x, 3); // 3 signal inlets: control, s1, s2

        attr_args_process(x, argc, argv);

        // Outlets are created from right to left in Max, but the code usually does them in the order they appear.
        // Wait, the standard in this repo is to create them from right to left?
        // No, the reviewer said "The leftmost outlet created with outlet_new corresponds to outs[0]".
        // So I'll create them in order: mix1, mix2, sum, busy, and then log.

        if (x->log) {
            x->log_outlet = outlet_new((t_object *)x, NULL);
        }
        outlet_new((t_object *)x, "signal"); // busy (Outlet 4 or 5)
        outlet_new((t_object *)x, "signal"); // sum (Outlet 3 or 4)
        outlet_new((t_object *)x, "signal"); // mix2 (Outlet 2 or 3)
        outlet_new((t_object *)x, "signal"); // mix1 (Outlet 1, leftmost, outs[0])

        // Wait, if I create them in this order, mix1 is created LAST, so it is the LEFTMOST.
        // If I create mix1 first, it will be the rightmost.
        // Max's outlet_new prepends the outlet.

        crossfade_init(&x->state, sys_getsr(), x->low_ms, x->high_ms);
    }
    return x;
}

void crossfade_free(t_crossfade *x) {
    dsp_free((t_pxobject *)x);
}

void crossfade_dsp64(t_crossfade *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    crossfade_update_params(&x->state, samplerate, x->low_ms, x->high_ms);
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)crossfade_perform64, 0, NULL);
}

void crossfade_perform64(t_crossfade *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *control = ins[0];
    double *s1 = ins[1];
    double *s2 = ins[2];
    double *mix1_out = outs[0];
    double *mix2_out = outs[1];
    double *sum_out = outs[2];
    double *busy_out = outs[3];

    // Update parameters in state (in case they changed via attribute messages)
    crossfade_update_params(&x->state, -1.0, x->low_ms, x->high_ms);

    for (int i = 0; i < sampleframes; i++) {
        double m1, m2, sum;
        int busy;
        crossfade_process(&x->state, control[i], s1[i], s2[i], &m1, &m2, &sum, &busy);
        mix1_out[i] = m1;
        mix2_out[i] = m2;
        sum_out[i] = sum;
        busy_out[i] = (double)busy;
    }
}

void crossfade_assist(t_crossfade *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(signal) Control"); break;
            case 1: sprintf(s, "(signal) Source 1"); break;
            case 2: sprintf(s, "(signal) Source 2"); break;
        }
    } else {
        if (x->log) {
            switch (a) {
                case 0: sprintf(s, "(signal) Mix 1"); break;
                case 1: sprintf(s, "(signal) Mix 2"); break;
                case 2: sprintf(s, "(signal) Sum"); break;
                case 3: sprintf(s, "(signal) Busy"); break;
                case 4: sprintf(s, "Logging Outlet"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "(signal) Mix 1"); break;
                case 1: sprintf(s, "(signal) Mix 2"); break;
                case 2: sprintf(s, "(signal) Sum"); break;
                case 3: sprintf(s, "(signal) Busy"); break;
            }
        }
    }
}

#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "../shared/crossfade.h"

typedef struct _crossfade {
    t_pxobject x_obj;
    t_crossfade_state state;
} t_crossfade;

void *crossfade_new(t_symbol *s, long argc, t_atom *argv);
void crossfade_free(t_crossfade *x);
void crossfade_dsp64(t_crossfade *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void crossfade_perform64(t_crossfade *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void crossfade_assist(t_crossfade *x, void *b, long m, long a, char *s);

static t_class *crossfade_class;

void ext_main(void *r) {
    t_class *c = class_new("crossfade~", (method)crossfade_new, (method)crossfade_free, sizeof(t_crossfade), 0L, A_GIMME, 0);

    class_addmethod(c, (method)crossfade_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)crossfade_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    crossfade_class = c;
}

void *crossfade_new(t_symbol *s, long argc, t_atom *argv) {
    t_crossfade *x = (t_crossfade *)object_alloc(crossfade_class);

    if (x) {
        dsp_setup((t_pxobject *)x, 3); // 3 signal inlets: control, s1, s2

        // Outlets are created from left to right
        outlet_new((t_object *)x, "signal"); // mix1 (Outlet 1, leftmost, outs[0])
        outlet_new((t_object *)x, "signal"); // mix2 (Outlet 2, outs[1])
        outlet_new((t_object *)x, "signal"); // sum (Outlet 3, outs[2])
        outlet_new((t_object *)x, "signal"); // busy (Outlet 4, rightmost, outs[3])

        crossfade_init(&x->state);
    }
    return x;
}

void crossfade_free(t_crossfade *x) {
    dsp_free((t_pxobject *)x);
}

void crossfade_dsp64(t_crossfade *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
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
        switch (a) {
            case 0: sprintf(s, "(signal) Mix 1"); break;
            case 1: sprintf(s, "(signal) Mix 2"); break;
            case 2: sprintf(s, "(signal) Sum"); break;
            case 3: sprintf(s, "(signal) Busy"); break;
        }
    }
}

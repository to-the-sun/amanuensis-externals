#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

typedef struct _vstplus {
    t_pxobject v_obj;
} t_vstplus;

void *vstplus_new(t_symbol *s, long argc, t_atom *argv);
void vstplus_free(t_vstplus *x);
void vstplus_assist(t_vstplus *x, void *b, long m, long a, char *s);
void vstplus_dsp64(t_vstplus *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void vstplus_perform64(t_vstplus *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

static t_class *vstplus_class;

void ext_main(void *r) {
    t_class *c = class_new("vst~+", (method)vstplus_new, (method)vstplus_free, sizeof(t_vstplus), 0L, A_GIMME, 0);

    class_addmethod(c, (method)vstplus_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)vstplus_assist, "assist", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    vstplus_class = c;
}

void *vstplus_new(t_symbol *s, long argc, t_atom *argv) {
    t_vstplus *x = (t_vstplus *)object_alloc(vstplus_class);
    if (x) {
        dsp_setup((t_pxobject *)x, 1);
        outlet_new(x, "signal");
    }
    return x;
}

void vstplus_free(t_vstplus *x) {
    dsp_free((t_pxobject *)x);
}

void vstplus_assist(t_vstplus *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "(signal) Input");
    } else {
        sprintf(s, "(signal) Output");
    }
}

void vstplus_dsp64(t_vstplus *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)vstplus_perform64, 0, NULL);
}

void vstplus_perform64(t_vstplus *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];
    double *out = outs[0];
    for (int i = 0; i < sampleframes; i++) {
        out[i] = in[i];
    }
}

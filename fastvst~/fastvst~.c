#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include <string.h>

typedef struct _fastvst {
    t_pxobject v_obj;
    long log;
} t_fastvst;

void* fastvst_new(t_symbol* s, long argc, t_atom *argv);
void fastvst_free(t_fastvst* x);
void fastvst_assist(t_fastvst* x, void* b, long m, long a, char* s);
void fastvst_dsp64(t_fastvst* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags);
void fastvst_perform64(t_fastvst* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void *userparam);
void fastvst_plug(t_fastvst* x, t_symbol* s, long argc, t_atom* argv);

static t_class* fastvst_class;

void ext_main(void* r) {
    t_class* c = class_new("fastvst~", (method)fastvst_new, (method)fastvst_free, sizeof(t_fastvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)fastvst_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)fastvst_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)fastvst_plug, "plug", A_GIMME, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_fastvst, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    fastvst_class = c;
}

void* fastvst_new(t_symbol* s, long argc, t_atom *argv) {
    t_fastvst* x = (t_fastvst*)object_alloc(fastvst_class);
    if (x) {
        x->log = 0;
        attr_args_process(x, argc, argv);

        dsp_setup((t_pxobject*)x, 16);
        for (int i = 0; i < 16; i++) {
            outlet_new(x, "signal");
        }
    }
    return x;
}

void fastvst_free(t_fastvst* x) {
    dsp_free((t_pxobject*)x);
}

void fastvst_plug(t_fastvst* x, t_symbol* s, long argc, t_atom* argv) {
    if (argc > 0 && atom_gettype(argv) == A_SYM) {
        t_symbol *path = atom_getsym(argv);
        object_post((t_object*)x, "fastvst~: plug message received with path: %s", path->s_name);
    } else {
        object_post((t_object*)x, "fastvst~: plug message received (no path)");
    }
}

void fastvst_dsp64(t_fastvst* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object*)x, (t_perfroutine64)fastvst_perform64, 0, NULL);
}

void fastvst_perform64(t_fastvst* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void *userparam) {
    for (int i = 0; i < 16 && i < numins && i < numouts; i++) {
        memcpy(outs[i], ins[i], sampleframes * sizeof(double));
    }
}

void fastvst_assist(t_fastvst* x, void* b, long m, long a, char* s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Signal Inlet %ld", a + 1);
    } else {
        sprintf(s, "Signal Outlet %ld", a + 1);
    }
}

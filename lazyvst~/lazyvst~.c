#include <windows.h>
#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include <stdarg.h>

typedef struct _lazyvst {
    t_pxobject x_obj;
    long log;
    void *log_outlet;
    HINSTANCE hLib; // VST library handle
} t_lazyvst;

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv);
void lazyvst_free(t_lazyvst *x);
void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s);
void lazyvst_plug(t_lazyvst *x, t_symbol *s);
void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

static t_class *lazyvst_class;

// Helper function to send verbose log messages with prefix
void lazyvst_log(t_lazyvst *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "lazyvst~", fmt, args);
    va_end(args);
}

void ext_main(void *r) {
    t_class *c = class_new("lazyvst~", (method)lazyvst_new, (method)lazyvst_free, sizeof(t_lazyvst), 0L, A_GIMME, 0);

    class_addmethod(c, (method)lazyvst_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)lazyvst_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)lazyvst_plug, "plug", A_SYM, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_lazyvst, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    lazyvst_class = c;
}

void *lazyvst_new(t_symbol *s, long argc, t_atom *argv) {
    t_lazyvst *x = (t_lazyvst *)object_alloc(lazyvst_class);

    if (x) {
        x->log = 0;
        x->hLib = NULL;

        dsp_setup((t_pxobject *)x, 16); // 16 signal inlets

        attr_args_process(x, argc, argv);

        // Outlets (right to left)
        x->log_outlet = outlet_new((t_object *)x, NULL);
        for (int i = 15; i >= 0; i--) {
            outlet_new((t_object *)x, "signal");
        }
    }
    return x;
}

void lazyvst_free(t_lazyvst *x) {
    dsp_free((t_pxobject *)x);
    if (x->hLib) {
        FreeLibrary(x->hLib);
        x->hLib = NULL;
    }
}

void lazyvst_plug(t_lazyvst *x, t_symbol *s) {
    if (x->hLib) {
        lazyvst_log(x, "unloading previous library");
        FreeLibrary(x->hLib);
        x->hLib = NULL;
    }

    lazyvst_log(x, "plug: attempting to load %s", s->s_name);
    x->hLib = LoadLibraryA(s->s_name);

    if (x->hLib) {
        lazyvst_log(x, "plug: successfully loaded %s", s->s_name);
    } else {
        object_error((t_object *)x, "plug: failed to load %s (Error code: %lu)", s->s_name, GetLastError());
    }
}

void lazyvst_dsp64(t_lazyvst *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)lazyvst_perform64, 0, NULL);
}

void lazyvst_perform64(t_lazyvst *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    for (int chan = 0; chan < numouts; chan++) {
        for (int i = 0; i < sampleframes; i++) {
            outs[chan][i] = 0.0;
        }
    }
}

void lazyvst_assist(t_lazyvst *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "(signal) Input %ld", a + 1);
    } else {
        if (a == 16) {
            sprintf(s, "Logging Outlet");
        } else if (a < 16) {
            sprintf(s, "(signal) Output %ld", a + 1);
        }
    }
}

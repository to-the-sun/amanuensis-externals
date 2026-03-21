#include "ext.h"
#include "ext_obex.h"

typedef struct _stoplight {
    t_object s_obj;
    void *outlet;
    void *proxy;
    long proxy_id;
} t_stoplight;

void *stoplight_new(t_symbol *s, long argc, t_atom *argv);
void stoplight_free(t_stoplight *x);
void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s);

void stoplight_bang(t_stoplight *x);
void stoplight_int(t_stoplight *x, t_atom_long n);
void stoplight_float(t_stoplight *x, double f);
void stoplight_list(t_stoplight *x, t_symbol *s, long argc, t_atom *argv);
void stoplight_anything(t_stoplight *x, t_symbol *s, long argc, t_atom *argv);

t_class *stoplight_class;

void ext_main(void *r) {
    t_class *c;

    common_symbols_init();

    c = class_new("stoplight", (method)stoplight_new, (method)stoplight_free, sizeof(t_stoplight), 0L, A_GIMME, 0);

    class_addmethod(c, (method)stoplight_bang, "bang", 0);
    class_addmethod(c, (method)stoplight_int, "int", A_LONG, 0);
    class_addmethod(c, (method)stoplight_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)stoplight_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)stoplight_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)stoplight_assist, "assist", A_CANT, 0);

    class_register(CLASS_BOX, c);
    stoplight_class = c;
}

void *stoplight_new(t_symbol *s, long argc, t_atom *argv) {
    t_stoplight *x = (t_stoplight *)object_alloc(stoplight_class);

    if (x) {
        x->proxy = proxy_new((t_object *)x, 1, &x->proxy_id);
        x->outlet = outlet_new((t_object *)x, NULL);
    }
    return (x);
}

void stoplight_free(t_stoplight *x) {
    if (x->proxy) {
        object_free(x->proxy);
    }
}

void stoplight_bang(t_stoplight *x) {
    if (proxy_getinlet((t_object *)x) == 0) {
        outlet_bang(x->outlet);
    }
}

void stoplight_int(t_stoplight *x, t_atom_long n) {
    if (proxy_getinlet((t_object *)x) == 0) {
        outlet_int(x->outlet, n);
    }
}

void stoplight_float(t_stoplight *x, double f) {
    if (proxy_getinlet((t_object *)x) == 0) {
        outlet_float(x->outlet, f);
    }
}

void stoplight_list(t_stoplight *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) == 0) {
        outlet_list(x->outlet, s, (short)argc, argv);
    }
}

void stoplight_anything(t_stoplight *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) == 0) {
        outlet_anything(x->outlet, s, (short)argc, argv);
    }
}

void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0:
                sprintf(s, "Inlet 0: Accepts anything to be passed through.");
                break;
            case 1:
                sprintf(s, "Inlet 1: Control (0 or non-zero).");
                break;
        }
    } else {
        sprintf(s, "Outlet 1: Passed-through Data.");
    }
}

#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include <string.h>

typedef struct _stoplight_msg {
    t_symbol *s;
    long argc;
    t_atom *argv;
} t_stoplight_msg;

typedef struct _stoplight_snapshot {
    t_stoplight_msg msg;
    struct _stoplight_snapshot *next;
} t_stoplight_snapshot;

typedef struct _stoplight {
    t_object x_obj;
    t_critical lock;
    long inlet_num;                 // Inlet index updated by proxy
    t_stoplight_snapshot *queue_head;
    t_stoplight_snapshot *queue_tail;
    long hold_state;                // Control inlet value (0 or non-zero)
    void *data_outlet;              // Data outlet (Index 0)
    void *proxy;                    // Control proxy (Inlet 1)
    long is_flushing;               // Recursion guard
} t_stoplight;

static t_class *stoplight_class = NULL;

// Forward declarations
void stoplight_clear_msg(t_stoplight_msg *msg);
void stoplight_copy_msg(t_stoplight_msg *dest, t_symbol *s, long ac, t_atom *av);
void stoplight_free_snapshot(t_stoplight_snapshot *snap);
void stoplight_output_msg(t_stoplight *x, t_stoplight_msg *m);
void stoplight_flush(t_stoplight *x);
void stoplight_store_data(t_stoplight *x, long inlet_idx, t_symbol *s, long ac, t_atom *av);

void stoplight_bang(t_stoplight *x);
void stoplight_int(t_stoplight *x, t_atom_long n);
void stoplight_float(t_stoplight *x, double f);
void stoplight_list(t_stoplight *x, t_symbol *s, long ac, t_atom *av);
void stoplight_anything(t_stoplight *x, t_symbol *s, long ac, t_atom *av);

void *stoplight_new(t_symbol *s, long argc, t_atom *argv);
void stoplight_free(t_stoplight *x);
void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s);

// --- Implementation ---

void stoplight_clear_msg(t_stoplight_msg *msg) {
    if (msg->argv) {
        for (long i = 0; i < msg->argc; i++) {
            if (atom_gettype(&msg->argv[i]) == A_OBJ) {
                object_release(atom_getobj(&msg->argv[i]));
            }
        }
        sysmem_freeptr(msg->argv);
        msg->argv = NULL;
    }
    msg->argc = 0;
    msg->s = NULL;
}

void stoplight_copy_msg(t_stoplight_msg *dest, t_symbol *s, long ac, t_atom *av) {
    stoplight_clear_msg(dest);
    dest->s = s;
    dest->argc = ac;
    if (ac > 0 && av) {
        dest->argv = (t_atom *)sysmem_newptr(sizeof(t_atom) * ac);
        if (dest->argv) {
            sysmem_copyptr(av, dest->argv, sizeof(t_atom) * ac);
            for (long i = 0; i < ac; i++) {
                if (atom_gettype(&dest->argv[i]) == A_OBJ) {
                    object_retain(atom_getobj(&dest->argv[i]));
                }
            }
        } else {
            dest->argc = 0;
        }
    }
}

void stoplight_free_snapshot(t_stoplight_snapshot *snap) {
    if (snap) {
        stoplight_clear_msg(&snap->msg);
        sysmem_freeptr(snap);
    }
}

void stoplight_output_msg(t_stoplight *x, t_stoplight_msg *m) {
    if (!m || !m->s) return;
    if (m->s == gensym("bang")) {
        outlet_bang(x->data_outlet);
    } else if (m->s == gensym("int")) {
        if (m->argc > 0 && m->argv) outlet_int(x->data_outlet, atom_getlong(m->argv));
        else outlet_bang(x->data_outlet);
    } else if (m->s == gensym("float")) {
        if (m->argc > 0 && m->argv) outlet_float(x->data_outlet, atom_getfloat(m->argv));
        else outlet_bang(x->data_outlet);
    } else if (m->s == gensym("list")) {
        outlet_list(x->data_outlet, NULL, (short)m->argc, m->argv);
    } else {
        outlet_anything(x->data_outlet, m->s, (short)m->argc, m->argv);
    }
}

void stoplight_flush(t_stoplight *x) {
    critical_enter(x->lock);
    if (x->is_flushing) {
        critical_exit(x->lock);
        return;
    }
    x->is_flushing = 1;

    while (x->queue_head) {
        if (x->hold_state != 0) break;

        t_stoplight_snapshot *snap = x->queue_head;
        x->queue_head = snap->next;
        if (!x->queue_head) x->queue_tail = NULL;

        critical_exit(x->lock);
        stoplight_output_msg(x, &snap->msg);
        stoplight_free_snapshot(snap);
        critical_enter(x->lock);
    }
    x->is_flushing = 0;
    critical_exit(x->lock);
}

void stoplight_store_data(t_stoplight *x, long inlet_idx, t_symbol *s, long ac, t_atom *av) {
    if (inlet_idx == 0) {
        critical_enter(x->lock);
        if (x->hold_state == 0) {
            t_stoplight_msg temp_m;
            temp_m.argv = NULL;
            stoplight_copy_msg(&temp_m, s, ac, av);
            critical_exit(x->lock);
            stoplight_output_msg(x, &temp_m);
            stoplight_clear_msg(&temp_m);
        } else {
            t_stoplight_snapshot *snap = (t_stoplight_snapshot *)sysmem_newptrclear(sizeof(t_stoplight_snapshot));
            if (snap) {
                stoplight_copy_msg(&snap->msg, s, ac, av);
                snap->next = NULL;
                if (x->queue_tail) {
                    x->queue_tail->next = snap;
                } else {
                    x->queue_head = snap;
                }
                x->queue_tail = snap;
            }
            critical_exit(x->lock);
        }
    } else if (inlet_idx == 1) {
        critical_enter(x->lock);
        long prev_state = x->hold_state;
        if (s == gensym("int") || s == gensym("float")) {
            if (ac > 0 && av) x->hold_state = (long)atom_getfloat(av);
            else x->hold_state = 0;
        } else {
            x->hold_state = (ac > 0) ? 1 : 0;
        }

        int trigger_flush = (prev_state != 0 && x->hold_state == 0);
        critical_exit(x->lock);

        if (trigger_flush) {
            stoplight_flush(x);
        }
    }
}

// Handlers
void stoplight_bang(t_stoplight *x) {
    long inlet = proxy_getinlet((t_object *)x);
    stoplight_store_data(x, inlet, gensym("bang"), 0, NULL);
}

void stoplight_int(t_stoplight *x, t_atom_long n) {
    long inlet = proxy_getinlet((t_object *)x);
    t_atom a;
    atom_setlong(&a, n);
    stoplight_store_data(x, inlet, gensym("int"), 1, &a);
}

void stoplight_float(t_stoplight *x, double f) {
    long inlet = proxy_getinlet((t_object *)x);
    t_atom a;
    atom_setfloat(&a, f);
    stoplight_store_data(x, inlet, gensym("float"), 1, &a);
}

void stoplight_list(t_stoplight *x, t_symbol *s, long ac, t_atom *av) {
    long inlet = proxy_getinlet((t_object *)x);
    stoplight_store_data(x, inlet, gensym("list"), ac, av);
}

void stoplight_anything(t_stoplight *x, t_symbol *s, long ac, t_atom *av) {
    long inlet = proxy_getinlet((t_object *)x);
    stoplight_store_data(x, inlet, s, ac, av);
}

// Object lifecycle
void *stoplight_new(t_symbol *s, long argc, t_atom *argv) {
    t_stoplight *x = (t_stoplight *)object_alloc(stoplight_class);
    if (x) {
        critical_new(&x->lock);
        x->inlet_num = 0;
        x->hold_state = 0;
        x->queue_head = NULL;
        x->queue_tail = NULL;
        x->is_flushing = 0;

        x->data_outlet = outlet_new((t_object *)x, NULL);
        x->proxy = proxy_new(x, 1, &x->inlet_num);

        attr_args_process(x, argc, argv);
    }
    return x;
}

void stoplight_free(t_stoplight *x) {
    if (x->proxy) object_free(x->proxy);

    while (x->queue_head) {
        t_stoplight_snapshot *snap = x->queue_head;
        x->queue_head = snap->next;
        stoplight_free_snapshot(snap);
    }
    critical_free(x->lock);
}

void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        if (a == 0) sprintf(s, "Data Inlet (Hot)");
        else sprintf(s, "Control Inlet (Hold if non-zero)");
    } else {
        sprintf(s, "Data Outlet");
    }
}

void ext_main(void *r) {
    common_symbols_init();

    t_class *c = class_new("stoplight", (method)stoplight_new, (method)stoplight_free, sizeof(t_stoplight), 0L, A_GIMME, 0);
    stoplight_class = c;

    class_addmethod(c, (method)stoplight_bang, "bang", 0);
    class_addmethod(c, (method)stoplight_int, "int", A_LONG, 0);
    class_addmethod(c, (method)stoplight_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)stoplight_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)stoplight_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)stoplight_assist, "assist", A_CANT, 0);

    class_register(CLASS_BOX, c);
}

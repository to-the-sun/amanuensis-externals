#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"

typedef enum _msg_type {
    MSG_BANG,
    MSG_INT,
    MSG_FLOAT,
    MSG_LIST,
    MSG_ANYTHING
} t_msg_type;

typedef struct _queued_msg {
    t_msg_type type;
    t_symbol *s;
    long argc;
    t_atom *argv;
} t_queued_msg;

typedef struct _stoplight {
    t_object s_obj;
    void *outlet;
    void *proxy;
    long proxy_id;
    t_atom_long state;
    t_linklist *queue;
    t_critical lock;
    long is_flushing;
} t_stoplight;

void *stoplight_new(t_symbol *s, long argc, t_atom *argv);
void stoplight_free(t_stoplight *x);
void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s);

void stoplight_queue_message(t_stoplight *x, t_msg_type type, t_symbol *s, long argc, t_atom *argv);
void stoplight_flush(t_stoplight *x);

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
        critical_new(&x->lock);
        x->state = 0;
        x->is_flushing = 0;
        x->queue = linklist_new();
        x->proxy = proxy_new((t_object *)x, 1, &x->proxy_id);
        x->outlet = outlet_new((t_object *)x, NULL);
    }
    return (x);
}

void stoplight_free(t_stoplight *x) {
    if (x->proxy) {
        object_free(x->proxy);
    }
    if (x->queue) {
        t_queued_msg *msg;
        while (linklist_getsize(x->queue) > 0) {
            msg = (t_queued_msg *)linklist_getindex(x->queue, 0);
            linklist_chuckindex(x->queue, 0);
            if (msg) {
                if (msg->argv) {
                    for (long i = 0; i < msg->argc; i++) {
                        if (atom_gettype(&msg->argv[i]) == A_OBJ) {
                            object_release(atom_getobj(&msg->argv[i]));
                        }
                    }
                    sysmem_freeptr(msg->argv);
                }
                sysmem_freeptr(msg);
            }
        }
        object_free(x->queue);
    }
    critical_free(x->lock);
}

void stoplight_queue_message(t_stoplight *x, t_msg_type type, t_symbol *s, long argc, t_atom *argv) {
    t_queued_msg *msg = (t_queued_msg *)sysmem_newptrclear(sizeof(t_queued_msg));
    if (msg) {
        msg->type = type;
        msg->s = s;
        msg->argc = argc;
        if (argc > 0 && argv) {
            msg->argv = (t_atom *)sysmem_newptrclear(argc * sizeof(t_atom));
            if (msg->argv) {
                for (long i = 0; i < argc; i++) {
                    msg->argv[i] = argv[i];
                    if (atom_gettype(&msg->argv[i]) == A_OBJ) {
                        object_retain(atom_getobj(&msg->argv[i]));
                    }
                }
            } else {
                sysmem_freeptr(msg);
                return;
            }
        } else {
            msg->argv = NULL;
        }
        critical_enter(x->lock);
        linklist_append(x->queue, msg);
        critical_exit(x->lock);
    }
}

void stoplight_flush(t_stoplight *x) {
    if (x->is_flushing) {
        return;
    }

    x->is_flushing = 1;

    while (x->state == 0 && linklist_getsize(x->queue) > 0) {
        t_queued_msg *msg;

        critical_enter(x->lock);
        msg = (t_queued_msg *)linklist_getindex(x->queue, 0);
        linklist_chuckindex(x->queue, 0);
        critical_exit(x->lock);

        if (msg) {
            switch (msg->type) {
                case MSG_BANG:
                    outlet_bang(x->outlet);
                    break;
                case MSG_INT:
                    outlet_int(x->outlet, atom_getlong(msg->argv));
                    break;
                case MSG_FLOAT:
                    outlet_float(x->outlet, atom_getfloat(msg->argv));
                    break;
                case MSG_LIST:
                    outlet_list(x->outlet, msg->s, (short)msg->argc, msg->argv);
                    break;
                case MSG_ANYTHING:
                    outlet_anything(x->outlet, msg->s, (short)msg->argc, msg->argv);
                    break;
            }

            if (msg->argv) {
                for (long i = 0; i < msg->argc; i++) {
                    if (atom_gettype(&msg->argv[i]) == A_OBJ) {
                        object_release(atom_getobj(&msg->argv[i]));
                    }
                }
                sysmem_freeptr(msg->argv);
            }
            sysmem_freeptr(msg);
        }
    }

    x->is_flushing = 0;
}

void stoplight_bang(t_stoplight *x) {
    if (proxy_getinlet((t_object *)x) == 0) {
        if (x->state) {
            stoplight_queue_message(x, MSG_BANG, NULL, 0, NULL);
        } else {
            outlet_bang(x->outlet);
        }
    }
}

void stoplight_int(t_stoplight *x, t_atom_long n) {
    if (proxy_getinlet((t_object *)x) == 1) {
        x->state = (n != 0);
        if (x->state == 0) {
            stoplight_flush(x);
        }
    } else {
        if (x->state) {
            t_atom a;
            atom_setlong(&a, n);
            stoplight_queue_message(x, MSG_INT, NULL, 1, &a);
        } else {
            outlet_int(x->outlet, n);
        }
    }
}

void stoplight_float(t_stoplight *x, double f) {
    if (proxy_getinlet((t_object *)x) == 1) {
        x->state = (f != 0.0);
        if (x->state == 0) {
            stoplight_flush(x);
        }
    } else {
        if (x->state) {
            t_atom a;
            atom_setfloat(&a, f);
            stoplight_queue_message(x, MSG_FLOAT, NULL, 1, &a);
        } else {
            outlet_float(x->outlet, f);
        }
    }
}

void stoplight_list(t_stoplight *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) == 0) {
        if (x->state) {
            stoplight_queue_message(x, MSG_LIST, s, argc, argv);
        } else {
            outlet_list(x->outlet, s, (short)argc, argv);
        }
    }
}

void stoplight_anything(t_stoplight *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) == 0) {
        if (x->state) {
            stoplight_queue_message(x, MSG_ANYTHING, s, argc, argv);
        } else {
            outlet_anything(x->outlet, s, (short)argc, argv);
        }
    }
}

void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0:
                sprintf(s, "Inlet 0: Data to be passed or queued.");
                break;
            case 1:
                sprintf(s, "Inlet 1: Control Signal (0=pass, non-zero=queue).");
                break;
        }
    } else {
        sprintf(s, "Outlet 1: Data Output.");
    }
}

#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "../shared/logging.h"
#include <string.h>

typedef struct _hold_msg {
    t_symbol *s;
    long argc;
    t_atom *argv;
} t_hold_msg;

typedef struct _hold_snapshot {
    t_hold_msg *msgs; // Array of N messages
    struct _hold_snapshot *next;
} t_hold_snapshot;

typedef struct _hold {
    t_object obj;
    long N;                         // Number of throughput inlets/outlets
    long inlet_num;                 // Inlet index updated by proxies
    t_hold_msg *current_inlets;     // Array of N messages for current state
    t_hold_snapshot *queue_head;
    t_hold_snapshot *queue_tail;
    long hold_state;                // Control inlet value (0 or non-zero)
    long log;                       // @log attribute
    void **through_outlets;         // Array of N outlets
    void **proxies;                 // Array of N proxies (Inlets 1 to N)
    long is_flushing;               // Recursion guard
    t_critical lock;
} t_hold;

static t_class *hold_class;

// Forward declarations
void hold_clear_msg(t_hold_msg *msg);
void hold_copy_msg(t_hold_msg *dest, t_symbol *s, long ac, t_atom *av);
void hold_free_snapshot(t_hold_snapshot *snap, long N);
void hold_output_snapshot(t_hold *x, t_hold_snapshot *snap);
void hold_flush(t_hold *x);
void hold_store_data(t_hold *x, long inlet_idx, t_symbol *s, long ac, t_atom *av);

void hold_bang(t_hold *x);
void hold_int(t_hold *x, long n);
void hold_float(t_hold *x, double f);
void hold_list(t_hold *x, t_symbol *s, long ac, t_atom *av);
void hold_anything(t_hold *x, t_symbol *s, long ac, t_atom *av);

void *hold_new(t_symbol *s, long argc, t_atom *argv);
void hold_free(t_hold *x);
void hold_assist(t_hold *x, void *b, long m, long a, char *s);

// --- Implementation ---

void hold_clear_msg(t_hold_msg *msg) {
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

void hold_copy_msg(t_hold_msg *dest, t_symbol *s, long ac, t_atom *av) {
    hold_clear_msg(dest);
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

void hold_free_snapshot(t_hold_snapshot *snap, long N) {
    if (snap) {
        if (snap->msgs) {
            for (long i = 0; i < N; i++) {
                hold_clear_msg(&snap->msgs[i]);
            }
            sysmem_freeptr(snap->msgs);
        }
        sysmem_freeptr(snap);
    }
}

void hold_output_snapshot(t_hold *x, t_hold_snapshot *snap) {
    // Right-to-left output order: Outlet N-1 down to 0
    for (long i = x->N - 1; i >= 0; i--) {
        t_hold_msg *m = &snap->msgs[i];
        if (m->s) {
            if (m->s == gensym("bang")) {
                outlet_bang(x->through_outlets[i]);
            } else if (m->s == gensym("int")) {
                if (m->argc > 0) outlet_int(x->through_outlets[i], atom_getlong(m->argv));
                else outlet_bang(x->through_outlets[i]);
            } else if (m->s == gensym("float")) {
                if (m->argc > 0) outlet_float(x->through_outlets[i], atom_getfloat(m->argv));
                else outlet_bang(x->through_outlets[i]);
            } else if (m->s == gensym("list")) {
                outlet_list(x->through_outlets[i], NULL, (short)m->argc, m->argv);
            } else {
                outlet_anything(x->through_outlets[i], m->s, (short)m->argc, m->argv);
            }
        }
    }
}

void hold_flush(t_hold *x) {
    critical_enter(x->lock);
    if (x->is_flushing) {
        critical_exit(x->lock);
        return;
    }
    x->is_flushing = 1;

    if (x->log) {
        common_log(NULL, x->log, "hold", "Flushing queue...");
    }
    while (x->queue_head) {
        // Peek at state to see if we should still be flushing
        if (x->hold_state != 0) break;

        t_hold_snapshot *snap = x->queue_head;
        x->queue_head = snap->next;
        if (!x->queue_head) x->queue_tail = NULL;

        critical_exit(x->lock);
        hold_output_snapshot(x, snap);
        hold_free_snapshot(snap, x->N);
        critical_enter(x->lock);
    }
    if (x->log) {
        common_log(NULL, x->log, "hold", "Flush complete.");
    }
    x->is_flushing = 0;
    critical_exit(x->lock);
}

void hold_store_data(t_hold *x, long inlet_idx, t_symbol *s, long ac, t_atom *av) {
    if (inlet_idx < x->N) {
        // Throughput inlet
        critical_enter(x->lock);
        hold_copy_msg(&x->current_inlets[inlet_idx], s, ac, av);

        if (inlet_idx == 0) {
            // Hot inlet
            if (x->hold_state == 0) {
                if (x->log) {
                    common_log(NULL, x->log, "hold", "Inlet 0 hot: Outputting current snapshot.");
                }
                // Output current state (safe copy)
                t_hold_snapshot *temp_snap = (t_hold_snapshot *)sysmem_newptr(sizeof(t_hold_snapshot));
                if (temp_snap) {
                    temp_snap->msgs = (t_hold_msg *)sysmem_newptr(sizeof(t_hold_msg) * x->N);
                    if (temp_snap->msgs) {
                        for (long i = 0; i < x->N; i++) {
                            temp_snap->msgs[i].argv = NULL;
                            hold_copy_msg(&temp_snap->msgs[i], x->current_inlets[i].s, x->current_inlets[i].argc, x->current_inlets[i].argv);
                        }
                        temp_snap->next = NULL;
                        critical_exit(x->lock);
                        hold_output_snapshot(x, temp_snap);
                        hold_free_snapshot(temp_snap, x->N);
                    } else {
                        sysmem_freeptr(temp_snap);
                        critical_exit(x->lock);
                    }
                } else {
                    critical_exit(x->lock);
                }
            } else {
                if (x->log) {
                    common_log(NULL, x->log, "hold", "Inlet 0 hot: Queueing current snapshot (hold active).");
                }
                // Queue current state
                t_hold_snapshot *snap = (t_hold_snapshot *)sysmem_newptr(sizeof(t_hold_snapshot));
                if (snap) {
                    snap->msgs = (t_hold_msg *)sysmem_newptr(sizeof(t_hold_msg) * x->N);
                    if (snap->msgs) {
                        for (long i = 0; i < x->N; i++) {
                            snap->msgs[i].argv = NULL;
                            hold_copy_msg(&snap->msgs[i], x->current_inlets[i].s, x->current_inlets[i].argc, x->current_inlets[i].argv);
                        }
                        snap->next = NULL;
                        if (x->queue_tail) {
                            x->queue_tail->next = snap;
                        } else {
                            x->queue_head = snap;
                        }
                        x->queue_tail = snap;
                    } else {
                        sysmem_freeptr(snap);
                    }
                }
                critical_exit(x->lock);
            }
        } else {
            critical_exit(x->lock);
        }
    } else if (inlet_idx == x->N) {
        // Control inlet
        critical_enter(x->lock);
        long prev_state = x->hold_state;
        if (s == gensym("int") || s == gensym("float")) {
            if (ac > 0) x->hold_state = (long)atom_getfloat(av);
            else x->hold_state = 0;
        } else {
            // Fallback for other messages on control inlet
            x->hold_state = (ac > 0) ? 1 : 0;
        }

        if (x->log) {
            common_log(NULL, x->log, "hold", "Control inlet: %ld", x->hold_state);
        }

        int trigger_flush = (prev_state != 0 && x->hold_state == 0);
        critical_exit(x->lock);

        if (trigger_flush) {
            hold_flush(x);
        }
    }
}

// Handlers
void hold_bang(t_hold *x) {
    long inlet = proxy_getinlet((t_object *)x);
    hold_store_data(x, inlet, gensym("bang"), 0, NULL);
}

void hold_int(t_hold *x, long n) {
    long inlet = proxy_getinlet((t_object *)x);
    t_atom a;
    atom_setlong(&a, n);
    hold_store_data(x, inlet, gensym("int"), 1, &a);
}

void hold_float(t_hold *x, double f) {
    long inlet = proxy_getinlet((t_object *)x);
    t_atom a;
    atom_setfloat(&a, f);
    hold_store_data(x, inlet, gensym("float"), 1, &a);
}

void hold_list(t_hold *x, t_symbol *s, long ac, t_atom *av) {
    long inlet = proxy_getinlet((t_object *)x);
    hold_store_data(x, inlet, gensym("list"), ac, av);
}

void hold_anything(t_hold *x, t_symbol *s, long ac, t_atom *av) {
    long inlet = proxy_getinlet((t_object *)x);
    hold_store_data(x, inlet, s, ac, av);
}

// Object lifecycle
void *hold_new(t_symbol *s, long argc, t_atom *argv) {
    t_hold *x = (t_hold *)object_alloc(hold_class);
    if (x) {
        long n = 1;
        if (argc > 0 && atom_gettype(argv) == A_LONG) {
            n = atom_getlong(argv);
            argc--;
            argv++;
        }
        if (n < 1) n = 1;
        x->N = n;
        x->inlet_num = 0;
        x->hold_state = 0;
        x->log = 0;
        x->queue_head = NULL;
        x->queue_tail = NULL;
        x->is_flushing = 0;
        critical_new(&x->lock);

        // Current inlets storage
        x->current_inlets = (t_hold_msg *)sysmem_newptr(sizeof(t_hold_msg) * x->N);
        for (long i = 0; i < x->N; i++) {
            x->current_inlets[i].s = gensym("bang");
            x->current_inlets[i].argc = 0;
            x->current_inlets[i].argv = NULL;
        }

        // Proxies for Inlets 1 to N
        x->proxies = (void **)sysmem_newptr(sizeof(void *) * x->N);
        for (long i = 0; i < x->N; i++) {
            x->proxies[i] = proxy_new(x, (long)(i + 1), &x->inlet_num);
        }

        // Outlets: Throughputs N-1 down to 0
        x->through_outlets = (void **)sysmem_newptr(sizeof(void *) * x->N);
        for (long i = x->N - 1; i >= 0; i--) {
            x->through_outlets[i] = outlet_new(x, NULL);
        }

        attr_args_process(x, argc, argv);
    }
    return x;
}

void hold_free(t_hold *x) {
    critical_free(x->lock);
    if (x->current_inlets) {
        for (long i = 0; i < x->N; i++) {
            hold_clear_msg(&x->current_inlets[i]);
        }
        sysmem_freeptr(x->current_inlets);
    }
    if (x->proxies) {
        for (long i = 0; i < x->N; i++) {
            object_free(x->proxies[i]);
        }
        sysmem_freeptr(x->proxies);
    }
    if (x->through_outlets) {
        sysmem_freeptr(x->through_outlets);
    }

    while (x->queue_head) {
        t_hold_snapshot *snap = x->queue_head;
        x->queue_head = snap->next;
        hold_free_snapshot(snap, x->N);
    }
}

void hold_assist(t_hold *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        if (a < x->N) {
            sprintf(s, "Throughput Inlet %ld (%s)", a, (a == 0) ? "Hot" : "Cold");
        } else {
            sprintf(s, "Control Inlet (Hold if non-zero)");
        }
    } else {
        sprintf(s, "Throughput Outlet %ld", a);
    }
}

void ext_main(void *r) {
    t_class *c = class_new("hold", (method)hold_new, (method)hold_free, sizeof(t_hold), 0L, A_GIMME, 0);

    class_addmethod(c, (method)hold_bang, "bang", 0);
    class_addmethod(c, (method)hold_int, "int", A_LONG, 0);
    class_addmethod(c, (method)hold_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)hold_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)hold_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)hold_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_hold, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    hold_class = c;
    common_symbols_init();
}

#include "ext.h"
#include "ext_obex.h"
#include "ext_critical.h"
#include "../shared/logging.h"
#include <stdarg.h>

#define MAX_PAIRS 9

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

typedef struct _bundle {
    t_queued_msg *msgs; // Array of size num_pairs
} t_bundle;

typedef struct _stoplight {
    t_object s_obj;
    long num_pairs;
    void **outlets;
    void **proxies;
    long *proxy_ids;
    t_atom_long state;
    t_linklist *queue;
    t_critical lock;
    long is_flushing;
    t_queued_msg *last_items; // Size num_pairs - 1 (for cold data inlets 1..N-1)
    long log;
    void *out_log;
} t_stoplight;

// Helper functions
void stoplight_log(t_stoplight *x, const char *fmt, ...);
void stoplight_format_bundle(t_stoplight *x, t_bundle *bundle, char *buf, size_t size);
void stoplight_copy_msg(t_queued_msg *dest, t_msg_type type, t_symbol *s, long argc, t_atom *argv);
void stoplight_clear_msg(t_queued_msg *msg);
void stoplight_output_msg(t_stoplight *x, void *outlet, t_queued_msg *msg);

void *stoplight_new(t_symbol *s, long argc, t_atom *argv);
void stoplight_free(t_stoplight *x);
t_max_err stoplight_attr_set_log(t_stoplight *x, void *attr, long ac, t_atom *av);
void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s);

void stoplight_queue_bundle(t_stoplight *x, t_msg_type type, t_symbol *s, long argc, t_atom *argv);
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

    CLASS_ATTR_LONG(c, "log", 0, t_stoplight, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "log", NULL, (method)stoplight_attr_set_log);

    class_register(CLASS_BOX, c);
    stoplight_class = c;
}

void stoplight_log(t_stoplight *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->out_log, x->log, "stoplight", fmt, args);
    va_end(args);
}

t_max_err stoplight_attr_set_log(t_stoplight *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->log = atom_getlong(av);
        post("stoplight: log attribute set to %ld", x->log);
    }
    return MAX_ERR_NONE;
}

void stoplight_format_bundle(t_stoplight *x, t_bundle *bundle, char *buf, size_t size) {
    if (!x || !bundle || !buf || size == 0) return;

    t_string *s = string_new("");
    if (!s) return;

    for (long i = 0; i < x->num_pairs; i++) {
        t_queued_msg *msg = &bundle->msgs[i];
        char *msg_str = NULL;
        long msg_size = 0;

        switch (msg->type) {
            case MSG_BANG:
                string_append(s, "bang");
                break;
            case MSG_INT:
            case MSG_FLOAT:
            case MSG_LIST:
                if (msg->argc > 0 && msg->argv) {
                    atom_gettext(msg->argc, msg->argv, &msg_size, &msg_str, OBEX_UTIL_ATOM_GETTEXT_DEFAULT);
                    if (msg_str) {
                        string_append(s, msg_str);
                        sysmem_freeptr(msg_str);
                    }
                }
                break;
            case MSG_ANYTHING:
                if (msg->s) {
                    string_append(s, msg->s->s_name);
                    if (msg->argc > 0 && msg->argv) {
                        string_append(s, " ");
                        atom_gettext(msg->argc, msg->argv, &msg_size, &msg_str, OBEX_UTIL_ATOM_GETTEXT_DEFAULT);
                        if (msg_str) {
                            string_append(s, msg_str);
                            sysmem_freeptr(msg_str);
                        }
                    }
                }
                break;
        }

        if (i < x->num_pairs - 1) {
            string_append(s, ", ");
        }
    }

    snprintf(buf, size, "%s", string_getptr(s));
    object_free(s);
}

void stoplight_copy_msg(t_queued_msg *dest, t_msg_type type, t_symbol *s, long argc, t_atom *argv) {
    dest->type = type;
    dest->s = s;
    dest->argc = argc;
    if (argc > 0 && argv) {
        dest->argv = (t_atom *)sysmem_newptrclear(argc * sizeof(t_atom));
        if (dest->argv) {
            for (long i = 0; i < argc; i++) {
                dest->argv[i] = argv[i];
                if (atom_gettype(&dest->argv[i]) == A_OBJ) {
                    object_retain(atom_getobj(&dest->argv[i]));
                }
            }
        }
    } else {
        dest->argv = NULL;
    }
}

void stoplight_clear_msg(t_queued_msg *msg) {
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
}

void stoplight_output_msg(t_stoplight *x, void *outlet, t_queued_msg *msg) {
    switch (msg->type) {
        case MSG_BANG:
            outlet_bang(outlet);
            break;
        case MSG_INT:
            outlet_int(outlet, atom_getlong(msg->argv));
            break;
        case MSG_FLOAT:
            outlet_float(outlet, atom_getfloat(msg->argv));
            break;
        case MSG_LIST:
            outlet_list(outlet, msg->s, (short)msg->argc, msg->argv);
            break;
        case MSG_ANYTHING:
            outlet_anything(outlet, msg->s, (short)msg->argc, msg->argv);
            break;
    }
}

void *stoplight_new(t_symbol *s, long argc, t_atom *argv) {
    t_stoplight *x = (t_stoplight *)object_alloc(stoplight_class);

    if (x) {
        x->num_pairs = 1;
        if (argc > 0 && atom_gettype(argv) == A_LONG) {
            long val = atom_getlong(argv);
            if (val > 1) {
                if (val > MAX_PAIRS) val = MAX_PAIRS;
                x->num_pairs = val;
            }
        }

        critical_new(&x->lock);
        x->state = 0;
        x->is_flushing = 0;
        x->log = 0;
        x->queue = linklist_new();

        // Outlets (create right-to-left for left-to-right appearance)
        x->out_log = outlet_new((t_object *)x, NULL);
        x->outlets = (void **)sysmem_newptrclear(x->num_pairs * sizeof(void *));
        for (long i = x->num_pairs - 1; i >= 0; i--) {
            x->outlets[i] = outlet_new((t_object *)x, NULL);
        }

        // Proxies (cold data inlets 1..N-1 and control inlet N)
        // Created right-to-left to appear left-to-right (Main, Cold1, ..., Control)
        x->proxies = (void **)sysmem_newptrclear(x->num_pairs * sizeof(void *));
        x->proxy_ids = (long *)sysmem_newptrclear(x->num_pairs * sizeof(long));
        for (long i = x->num_pairs; i >= 1; i--) {
            x->proxy_ids[i - 1] = i;
            x->proxies[i - 1] = proxy_new((t_object *)x, x->proxy_ids[i - 1], &x->proxy_ids[i - 1]);
        }

        // Last items for cold data inlets
        if (x->num_pairs > 1) {
            x->last_items = (t_queued_msg *)sysmem_newptrclear((x->num_pairs - 1) * sizeof(t_queued_msg));
            for (long i = 0; i < x->num_pairs - 1; i++) {
                // Initialize with integer 0
                t_atom a;
                atom_setlong(&a, 0);
                stoplight_copy_msg(&x->last_items[i], MSG_INT, NULL, 1, &a);
            }
        } else {
            x->last_items = NULL;
        }

        attr_args_process(x, argc, argv);
    }
    return (x);
}

void stoplight_free(t_stoplight *x) {
    if (x->proxies) {
        for (long i = 0; i < x->num_pairs; i++) {
            if (x->proxies[i]) {
                object_free(x->proxies[i]);
            }
        }
        sysmem_freeptr(x->proxies);
    }
    if (x->proxy_ids) {
        sysmem_freeptr(x->proxy_ids);
    }
    if (x->outlets) {
        sysmem_freeptr(x->outlets);
    }
    if (x->last_items) {
        for (long i = 0; i < x->num_pairs - 1; i++) {
            stoplight_clear_msg(&x->last_items[i]);
        }
        sysmem_freeptr(x->last_items);
    }
    if (x->queue) {
        t_bundle *bundle;
        while (linklist_getsize(x->queue) > 0) {
            bundle = (t_bundle *)linklist_getindex(x->queue, 0);
            linklist_chuckindex(x->queue, 0);
            if (bundle) {
                if (bundle->msgs) {
                    for (long i = 0; i < x->num_pairs; i++) {
                        stoplight_clear_msg(&bundle->msgs[i]);
                    }
                    sysmem_freeptr(bundle->msgs);
                }
                sysmem_freeptr(bundle);
            }
        }
        object_free(x->queue);
    }
    critical_free(x->lock);
}

void stoplight_queue_bundle(t_stoplight *x, t_msg_type type, t_symbol *s, long argc, t_atom *argv) {
    t_bundle *bundle = (t_bundle *)sysmem_newptrclear(sizeof(t_bundle));
    if (bundle) {
        bundle->msgs = (t_queued_msg *)sysmem_newptrclear(x->num_pairs * sizeof(t_queued_msg));
        if (bundle->msgs) {
            // Inlet 0: current item
            stoplight_copy_msg(&bundle->msgs[0], type, s, argc, argv);
            // Inlets 1..N-1: last items
            for (long i = 1; i < x->num_pairs; i++) {
                t_queued_msg *src = &x->last_items[i-1];
                stoplight_copy_msg(&bundle->msgs[i], src->type, src->s, src->argc, src->argv);
            }

            critical_enter(x->lock);
            linklist_append(x->queue, bundle);
            long size = linklist_getsize(x->queue);
            critical_exit(x->lock);

            char bundle_buf[1024];
            stoplight_format_bundle(x, bundle, bundle_buf, sizeof(bundle_buf));
            stoplight_log(x, "Bundle [%s] added to queue. Queue size: %ld", bundle_buf, size);
        } else {
            sysmem_freeptr(bundle);
        }
    }
}

void stoplight_flush(t_stoplight *x) {
    if (x->is_flushing) {
        return;
    }

    x->is_flushing = 1;

    while (x->state == 0 && linklist_getsize(x->queue) > 0) {
        t_bundle *bundle;

        critical_enter(x->lock);
        bundle = (t_bundle *)linklist_getindex(x->queue, 0);
        linklist_chuckindex(x->queue, 0);
        long size = linklist_getsize(x->queue);
        critical_exit(x->lock);

        if (bundle) {
            char bundle_buf[1024];
            stoplight_format_bundle(x, bundle, bundle_buf, sizeof(bundle_buf));
            stoplight_log(x, "Bundle [%s] released from queue. Remaining size: %ld", bundle_buf, size);
            // Output in right-to-left order (Outlet N-1 down to 0)
            for (long i = x->num_pairs - 1; i >= 0; i--) {
                stoplight_output_msg(x, x->outlets[i], &bundle->msgs[i]);
            }

            if (bundle->msgs) {
                for (long i = 0; i < x->num_pairs; i++) {
                    stoplight_clear_msg(&bundle->msgs[i]);
                }
                sysmem_freeptr(bundle->msgs);
            }
            sysmem_freeptr(bundle);
        }
    }

    x->is_flushing = 0;
}

void stoplight_bang(t_stoplight *x) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 0) {
        if (x->state) {
            stoplight_queue_bundle(x, MSG_BANG, NULL, 0, NULL);
        } else {
            // Right-to-left output
            for (long i = x->num_pairs - 1; i >= 1; i--) {
                stoplight_output_msg(x, x->outlets[i], &x->last_items[i-1]);
            }
            outlet_bang(x->outlets[0]);
        }
    } else if (inlet < x->num_pairs) {
        // Cold data inlet
        stoplight_clear_msg(&x->last_items[inlet - 1]);
        stoplight_copy_msg(&x->last_items[inlet - 1], MSG_BANG, NULL, 0, NULL);
    }
}

void stoplight_int(t_stoplight *x, t_atom_long n) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == x->num_pairs) {
        // Control inlet
        x->state = (n != 0);
        if (x->state == 0) {
            stoplight_flush(x);
        }
    } else if (inlet == 0) {
        t_atom a;
        atom_setlong(&a, n);
        if (x->state) {
            stoplight_queue_bundle(x, MSG_INT, NULL, 1, &a);
        } else {
            // Right-to-left output
            for (long i = x->num_pairs - 1; i >= 1; i--) {
                stoplight_output_msg(x, x->outlets[i], &x->last_items[i-1]);
            }
            outlet_int(x->outlets[0], n);
        }
    } else {
        // Cold data inlet
        stoplight_clear_msg(&x->last_items[inlet - 1]);
        t_atom a;
        atom_setlong(&a, n);
        stoplight_copy_msg(&x->last_items[inlet - 1], MSG_INT, NULL, 1, &a);
    }
}

void stoplight_float(t_stoplight *x, double f) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == x->num_pairs) {
        // Control inlet
        x->state = (f != 0.0);
        if (x->state == 0) {
            stoplight_flush(x);
        }
    } else if (inlet == 0) {
        t_atom a;
        atom_setfloat(&a, f);
        if (x->state) {
            stoplight_queue_bundle(x, MSG_FLOAT, NULL, 1, &a);
        } else {
            // Right-to-left output
            for (long i = x->num_pairs - 1; i >= 1; i--) {
                stoplight_output_msg(x, x->outlets[i], &x->last_items[i-1]);
            }
            outlet_float(x->outlets[0], f);
        }
    } else {
        // Cold data inlet
        stoplight_clear_msg(&x->last_items[inlet - 1]);
        t_atom a;
        atom_setfloat(&a, f);
        stoplight_copy_msg(&x->last_items[inlet - 1], MSG_FLOAT, NULL, 1, &a);
    }
}

void stoplight_list(t_stoplight *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 0) {
        if (x->state) {
            stoplight_queue_bundle(x, MSG_LIST, s, argc, argv);
        } else {
            // Right-to-left output
            for (long i = x->num_pairs - 1; i >= 1; i--) {
                stoplight_output_msg(x, x->outlets[i], &x->last_items[i-1]);
            }
            outlet_list(x->outlets[0], s, (short)argc, argv);
        }
    } else if (inlet < x->num_pairs) {
        // Cold data inlet
        stoplight_clear_msg(&x->last_items[inlet - 1]);
        stoplight_copy_msg(&x->last_items[inlet - 1], MSG_LIST, s, argc, argv);
    }
}

void stoplight_anything(t_stoplight *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 0) {
        if (x->state) {
            stoplight_queue_bundle(x, MSG_ANYTHING, s, argc, argv);
        } else {
            // Right-to-left output
            for (long i = x->num_pairs - 1; i >= 1; i--) {
                stoplight_output_msg(x, x->outlets[i], &x->last_items[i-1]);
            }
            outlet_anything(x->outlets[0], s, (short)argc, argv);
        }
    } else if (inlet < x->num_pairs) {
        // Cold data inlet
        stoplight_clear_msg(&x->last_items[inlet - 1]);
        stoplight_copy_msg(&x->last_items[inlet - 1], MSG_ANYTHING, s, argc, argv);
    }
}

void stoplight_assist(t_stoplight *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        if (a == 0) {
            sprintf(s, "Inlet 1 (Hot): Data to be passed or queued.");
        } else if (a == x->num_pairs) {
            sprintf(s, "Inlet %ld (Control): Control Signal (0=pass, non-zero=queue).", a + 1);
        } else {
            sprintf(s, "Inlet %ld (Cold): Data to be bundled with Inlet 1.", a + 1);
        }
    } else {
        if (a < x->num_pairs) {
            sprintf(s, "Outlet %ld: Data Output.", a + 1);
        } else {
            sprintf(s, "Outlet %ld: Logging and Status messages", a + 1);
        }
    }
}

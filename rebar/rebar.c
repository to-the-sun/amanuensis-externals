#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include "../shared/visualize.h"

#include <string.h>
#include <stdarg.h>

// Forward declarations of the modules' structs
struct _notify;
struct _buildspans;
struct _crucible;

typedef struct _rebar {
    t_object s_obj;
    struct _notify *notify_inst;
    struct _buildspans *buildspans_inst;
    struct _crucible *crucible_inst;

    void *out_data;      // Crucible Outlet 0
    void *out_fill;      // Crucible Outlet 1
    void *out_reach;     // Crucible Outlet 2
    void *out_log;       // Consolidated logging

    long log;
    long defer;
    long consume;
    long visualize;

    t_symbol *user_dict_name;
    t_symbol *tmp_dict_name;
} t_rebar;

static t_class *rebar_class;

// --- Global Context for Thread-Safe Instantiation ---
static t_critical g_rebar_crit;
static t_rebar *g_instantiating_rebar = NULL;
static int g_current_mod_hint = 0; // 0=notify, 1=buildspans, 2=crucible

// Capture real Max SDK functions
typedef void *(*t_outlet_new_fn)(t_object *x, char *classname);
typedef void *(*t_bangout_fn)(t_object *x);
typedef void *(*t_intout_fn)(t_object *x);
typedef void *(*t_floatout_fn)(t_object *x);
typedef void *(*t_listout_fn)(t_object *x);

// These will be initialized in ext_main
static t_outlet_new_fn sdk_outlet_new = NULL;
static t_bangout_fn sdk_bangout = NULL;
static t_intout_fn sdk_intout = NULL;
static t_floatout_fn sdk_floatout = NULL;
static t_listout_fn sdk_listout = NULL;

// --- Virtual Outlet Management ---

typedef enum {
    MOD_NOTIFY,
    MOD_BUILDSPANS,
    MOD_CRUCIBLE
} t_mod_type;

typedef struct {
    void *owner;
    t_mod_type type;
    int index;
} t_virt_outlet;

typedef struct _outlet_node {
    t_virt_outlet vo;
    struct _outlet_node *next;
} t_outlet_node;

static t_outlet_node *g_outlet_registry = NULL;

static void *register_virt_outlet(void *owner, t_mod_type type) {
    critical_enter(g_rebar_crit);
    int idx = 0;
    t_outlet_node *curr = g_outlet_registry;
    while (curr) {
        if (curr->vo.owner == owner) idx++;
        curr = curr->next;
    }

    t_outlet_node *node = (t_outlet_node *)sysmem_newptr(sizeof(t_outlet_node));
    node->vo.owner = owner;
    node->vo.type = type;
    node->vo.index = idx;
    node->next = g_outlet_registry;
    g_outlet_registry = node;

    void *fake = (void *)node;
    critical_exit(g_rebar_crit);
    return fake;
}

static t_virt_outlet *get_virt_outlet(void *fake) {
    t_virt_outlet *vo = NULL;
    critical_enter(g_rebar_crit);
    t_outlet_node *curr = g_outlet_registry;
    while (curr) {
        if ((void *)curr == fake) {
            vo = &curr->vo;
            break;
        }
        curr = curr->next;
    }
    critical_exit(g_rebar_crit);
    return vo;
}

static void unregister_outlets(void *owner) {
    critical_enter(g_rebar_crit);
    t_outlet_node **curr = &g_outlet_registry;
    while (*curr) {
        if ((*curr)->vo.owner == owner) {
            t_outlet_node *to_free = *curr;
            *curr = (*curr)->next;
            sysmem_freeptr(to_free);
        } else {
            curr = &((*curr)->next);
        }
    }
    critical_exit(g_rebar_crit);
}

// --- Module Registry ---

typedef struct _mod_map {
    void *module;
    t_rebar *rebar;
    struct _mod_map *next;
} t_mod_map;

static t_mod_map *g_mod_map = NULL;

static void register_module(void *module, t_rebar *rebar) {
    critical_enter(g_rebar_crit);
    t_mod_map *node = (t_mod_map *)sysmem_newptr(sizeof(t_mod_map));
    node->module = module;
    node->rebar = rebar;
    node->next = g_mod_map;
    g_mod_map = node;
    critical_exit(g_rebar_crit);
}

static void unregister_module(void *module) {
    critical_enter(g_rebar_crit);
    t_mod_map **curr = &g_mod_map;
    while (*curr) {
        if ((*curr)->module == module) {
            t_mod_map *to_free = *curr;
            *curr = (*curr)->next;
            sysmem_freeptr(to_free);
        } else {
            curr = &((*curr)->next);
        }
    }
    critical_exit(g_rebar_crit);
}

static t_rebar *get_rebar(void *module) {
    t_rebar *x = NULL;
    critical_enter(g_rebar_crit);
    t_mod_map *curr = g_mod_map;
    while (curr) {
        if (curr->module == module) {
            x = curr->rebar;
            break;
        }
        curr = curr->next;
    }
    critical_exit(g_rebar_crit);
    return x;
}

// --- Interceptor Prototypes ---

void *rebar_intercept_outlet_new(void *x, const char *classname);
void *rebar_intercept_bangout(void *x);
void *rebar_intercept_intout(void *x);
void *rebar_intercept_floatout(void *x);
void *rebar_intercept_listout(void *x);

void rebar_intercept_outlet_anything(void *o, t_symbol *s, short ac, t_atom *av);
void rebar_intercept_outlet_list(void *o, t_symbol *s, short ac, t_atom *av);
void rebar_intercept_outlet_int(void *o, t_atom_long n);
void rebar_intercept_outlet_float(void *o, double f);
void rebar_intercept_outlet_bang(void *o);

void *rebar_intercept_class_new(const char *name, method newmethod, method freemethod, size_t size, method menu_open_method, unsigned int type, ...);

// --- Coordination Prototypes ---
void rebar_request_copy_back(t_rebar *x);
void rebar_do_copy_back(t_rebar *x);

// --- Symbol Renaming ---

#define notify_class rebar_notify_class
#define note_compare rebar_note_compare
#define bar_key_compare rebar_bar_key_compare
#define notify_new rebar_notify_new
#define notify_free rebar_notify_free
#define notify_bang rebar_notify_bang
#define notify_int rebar_notify_int
#define notify_qwork rebar_notify_qwork
#define notify_do_bang rebar_notify_do_bang
#define notify_do_fill rebar_notify_do_fill
#define notify_assist rebar_notify_assist
#define notify_log rebar_notify_log

#define buildspans_class rebar_buildspans_class
#define compare_longs rebar_compare_longs
#define compare_notepairs rebar_compare_notepairs
#define compare_manifest_items rebar_compare_manifest_items
#define buildspans_new rebar_buildspans_new
#define buildspans_free rebar_buildspans_free
#define buildspans_clear rebar_buildspans_clear
#define buildspans_list rebar_buildspans_list
#define buildspans_offset rebar_buildspans_offset
#define buildspans_track rebar_buildspans_track
#define buildspans_track_deferred rebar_buildspans_track_deferred
#define buildspans_offset_deferred rebar_buildspans_offset_deferred
#define buildspans_anything rebar_buildspans_anything
#define buildspans_do_anything rebar_buildspans_do_anything
#define buildspans_anything_deferred rebar_buildspans_anything_deferred
#define buildspans_assist rebar_buildspans_assist
#define buildspans_bang module_buildspans_bang
#define buildspans_flush rebar_buildspans_flush
#define buildspans_end_track_span rebar_buildspans_end_track_span
#define buildspans_prune_span rebar_buildspans_prune_span
#define buildspans_visualize_memory rebar_buildspans_visualize_memory
#define buildspans_log rebar_buildspans_log
#define buildspans_reset_bar_to_standalone rebar_buildspans_reset_bar_to_standalone
#define buildspans_finalize_and_log_span rebar_buildspans_finalize_and_log_span
#define buildspans_deferred_rating_check rebar_buildspans_deferred_rating_check
#define buildspans_process_and_add_note rebar_buildspans_process_and_add_note
#define buildspans_cleanup_track_offset_if_needed rebar_buildspans_cleanup_track_offset_if_needed
#define find_next_offset rebar_find_next_offset
#define buildspans_validate_span_before_output rebar_buildspans_validate_span_before_output
#define buildspans_output_span_data rebar_buildspans_output_span_data
#define buildspans_get_bar_length rebar_buildspans_get_bar_length
#define buildspans_set_bar_buffer rebar_buildspans_set_bar_buffer
#define buildspans_local_bar_length rebar_buildspans_local_bar_length
#define atomarray_to_string rebar_atomarray_to_string
#define atomarray_deep_copy rebar_atomarray_deep_copy
#define parse_hierarchical_key rebar_parse_hierarchical_key
#define generate_hierarchical_key rebar_generate_hierarchical_key
#define compare_doubles rebar_compare_doubles

#define crucible_class rebar_crucible_class
#define crucible_new rebar_crucible_new
#define crucible_free rebar_crucible_free
#define crucible_anything rebar_crucible_anything
#define crucible_process_span rebar_crucible_process_span
#define crucible_assist rebar_crucible_assist
#define crucible_log rebar_crucible_log
#define crucible_atoms_to_string rebar_crucible_atoms_to_string
#define parse_selector rebar_parse_selector
#define dictionary_deep_copy rebar_dictionary_deep_copy
#define crucible_output_bar_data rebar_crucible_output_bar_data
#define crucible_local_bar_length rebar_crucible_local_bar_length
#define crucible_get_bar_length rebar_crucible_get_bar_length
#define crucible_get_span_as_atomarray rebar_crucible_get_span_as_atomarray
#define crucible_span_has_loser rebar_crucible_span_has_loser

// --- Redefine Max API ---

#define outlet_new(x, c) rebar_intercept_outlet_new(x, c)
#define bangout(x) rebar_intercept_bangout(x)
#define intout(x) rebar_intercept_intout(x)
#define floatout(x) rebar_intercept_floatout(x)
#define listout(x) rebar_intercept_listout(x)

#define outlet_anything(o, s, ac, av) rebar_intercept_outlet_anything(o, s, ac, av)
#define outlet_list(o, s, ac, av) rebar_intercept_outlet_list(o, s, ac, av)
#define outlet_int(o, n) rebar_intercept_outlet_int(o, n)
#define outlet_float(o, f) rebar_intercept_outlet_float(o, f)
#define outlet_bang(o) rebar_intercept_outlet_bang(o)

#define class_new rebar_intercept_class_new

// --- Include Modules ---

#define ext_main notify_main
#include "../notify/notify.c"
#undef ext_main

#define ext_main buildspans_main
#include "../buildspans/buildspans.c"
#undef ext_main

#define ext_main crucible_main
#include "../crucible/crucible.c"
#undef ext_main

// --- Interceptor Implementations ---

void *rebar_intercept_outlet_new(void *x, const char *classname) {
    critical_enter(g_rebar_crit);
    t_rebar *rebar = g_instantiating_rebar;
    critical_exit(g_rebar_crit);

    if (!rebar) return sdk_outlet_new((t_object *)x, (char *)classname);
    if (x == (void *)rebar) return sdk_outlet_new((t_object *)x, (char *)classname);

    return register_virt_outlet(x, (t_mod_type)g_current_mod_hint);
}

void *rebar_intercept_bangout(void *x) { return rebar_intercept_outlet_new(x, NULL); }
void *rebar_intercept_intout(void *x) { return rebar_intercept_outlet_new(x, NULL); }
void *rebar_intercept_floatout(void *x) { return rebar_intercept_outlet_new(x, NULL); }
void *rebar_intercept_listout(void *x) { return rebar_intercept_outlet_new(x, NULL); }

void *rebar_intercept_class_new(const char *name, method newmethod, method freemethod, size_t size, method menu_open_method, unsigned int type, ...) {
    char private_name[256];
    snprintf(private_name, 256, "rebar_%s_internal", name);
    #undef class_new
    t_class *c = class_new(private_name, newmethod, freemethod, size, menu_open_method, type, 0);
    #define class_new rebar_intercept_class_new
    return c;
}

// Wrapper for buildspans_bang (which we renamed to module_buildspans_bang)
void rebar_buildspans_bang(t_buildspans *x) {
    module_buildspans_bang(x);
    // When module_buildspans_bang returns, its immediate work (including synchronous crucible) is done.
    // If it deferred itself, the recursive call to module_buildspans_bang will also land here.
    if (systhread_ismainthread()) {
        t_rebar *rebar = get_rebar(x);
        if (rebar) rebar_request_copy_back(rebar);
    }
}

void rebar_intercept_outlet_anything(void *o, t_symbol *s, short ac, t_atom *av) {
    t_virt_outlet *vo = get_virt_outlet(o);
    if (!vo) { outlet_anything(o, s, ac, av); return; }
    t_rebar *x = get_rebar(vo->owner);
    if (!x) return;

    if (vo->type == MOD_NOTIFY) {
        if (vo->index == 3) rebar_buildspans_do_anything(x->buildspans_inst, s, (long)ac, av, 3);
        else if (vo->index == 4 && x->out_log) outlet_anything(x->out_log, s, ac, av);
    } else if (vo->type == MOD_BUILDSPANS) {
        if (vo->index == 2) rebar_crucible_anything(x->crucible_inst, s, (long)ac, av);
        else if (vo->index == 3 && x->out_log) outlet_anything(x->out_log, s, ac, av);
    } else if (vo->type == MOD_CRUCIBLE) {
        if (vo->index == 0) outlet_anything(x->out_data, s, ac, av);
        else if (vo->index == 1) outlet_anything(x->out_fill, s, ac, av);
        else if (vo->index == 3 && x->out_log) outlet_anything(x->out_log, s, ac, av);
    }
}

void rebar_intercept_outlet_list(void *o, t_symbol *s, short ac, t_atom *av) {
    t_virt_outlet *vo = get_virt_outlet(o);
    if (!vo) { outlet_list(o, s, ac, av); return; }
    t_rebar *x = get_rebar(vo->owner);
    if (!x) return;

    if (vo->type == MOD_NOTIFY) {
        if (vo->index == 0) rebar_buildspans_list(x->buildspans_inst, s, (long)ac, av);
    } else if (vo->type == MOD_CRUCIBLE) {
        if (vo->index == 0) outlet_list(x->out_data, s, ac, av);
    }
}

void rebar_intercept_outlet_int(void *o, t_atom_long n) {
    t_virt_outlet *vo = get_virt_outlet(o);
    if (!vo) { outlet_int(o, n); return; }
    t_rebar *x = get_rebar(vo->owner);
    if (!x) return;

    if (vo->type == MOD_NOTIFY) {
        if (vo->index == 2) rebar_buildspans_track(x->buildspans_inst, (long)n);
    } else if (vo->type == MOD_CRUCIBLE) {
        if (vo->index == 2) outlet_int(x->out_reach, n);
    }
}

void rebar_intercept_outlet_float(void *o, double f) {
    t_virt_outlet *vo = get_virt_outlet(o);
    if (!vo) { outlet_float(o, f); return; }
    t_rebar *x = get_rebar(vo->owner);
    if (!x) return;

    if (vo->type == MOD_NOTIFY) {
        if (vo->index == 1) rebar_buildspans_offset(x->buildspans_inst, f);
    }
}

void rebar_intercept_outlet_bang(void *o) {
    t_virt_outlet *vo = get_virt_outlet(o);
    if (!vo) { outlet_bang(o); return; }
    t_rebar *x = get_rebar(vo->owner);
    if (!x) return;

    if (vo->type == MOD_NOTIFY) {
        if (vo->index == 0) rebar_buildspans_bang(x->buildspans_inst);
    }
}

// --- Rebar Object Methods ---

void *rebar_new(t_symbol *s, long argc, t_atom *argv);
void rebar_free(t_rebar *x);
void rebar_int(t_rebar *x, long n);
void rebar_assist(t_rebar *x, void *b, long m, long a, char *s);

t_max_err rebar_attr_set_log(t_rebar *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->log = atom_getlong(av);
        if (x->notify_inst) x->notify_inst->log = x->log;
        if (x->buildspans_inst) x->buildspans_inst->log = x->log;
        if (x->crucible_inst) x->crucible_inst->log = x->log;
    }
    return MAX_ERR_NONE;
}

t_max_err rebar_attr_set_defer(t_rebar *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->defer = atom_getlong(av);
        if (x->notify_inst) x->notify_inst->defer = x->defer;
        if (x->buildspans_inst) x->buildspans_inst->defer = x->defer;
        if (x->crucible_inst) x->crucible_inst->defer = x->defer;
    }
    return MAX_ERR_NONE;
}

t_max_err rebar_attr_set_consume(t_rebar *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->consume = atom_getlong(av);
        if (x->crucible_inst) x->crucible_inst->consume = x->consume;
    }
    return MAX_ERR_NONE;
}

t_max_err rebar_attr_set_visualize(t_rebar *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->visualize = atom_getlong(av);
        if (x->buildspans_inst) x->buildspans_inst->visualize = x->visualize;
    }
    return MAX_ERR_NONE;
}

void ext_main(void *r) {
    critical_new(&g_rebar_crit);
    common_symbols_init();

    sdk_outlet_new = (t_outlet_new_fn)object_getmethod(gensym("outlet_new")->s_thing, gensym("outlet_new"));
    sdk_bangout = (t_bangout_fn)object_getmethod(gensym("bangout")->s_thing, gensym("bangout"));
    sdk_intout = (t_intout_fn)object_getmethod(gensym("intout")->s_thing, gensym("intout"));
    sdk_floatout = (t_floatout_fn)object_getmethod(gensym("floatout")->s_thing, gensym("floatout"));
    sdk_listout = (t_listout_fn)object_getmethod(gensym("listout")->s_thing, gensym("listout"));

    #undef class_new
    t_class *c = class_new("rebar", (method)rebar_new, (method)rebar_free, sizeof(t_rebar), 0L, A_GIMME, 0);
    #define class_new rebar_intercept_class_new

    class_addmethod(c, (method)rebar_int, "int", A_LONG, 0);
    class_addmethod(c, (method)rebar_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_rebar, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "log", NULL, (method)rebar_attr_set_log);

    CLASS_ATTR_LONG(c, "defer", 0, t_rebar, defer);
    CLASS_ATTR_STYLE_LABEL(c, "defer", 0, "onoff", "Deferred Execution");
    CLASS_ATTR_DEFAULT(c, "defer", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "defer", NULL, (method)rebar_attr_set_defer);

    CLASS_ATTR_LONG(c, "consume", 0, t_rebar, consume);
    CLASS_ATTR_STYLE_LABEL(c, "consume", 0, "onoff", "Enable Consume (Crucible)");
    CLASS_ATTR_DEFAULT(c, "consume", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "consume", NULL, (method)rebar_attr_set_consume);

    CLASS_ATTR_LONG(c, "visualize", 0, t_rebar, visualize);
    CLASS_ATTR_STYLE_LABEL(c, "visualize", 0, "onoff", "Enable Visualization (Buildspans)");
    CLASS_ATTR_DEFAULT(c, "visualize", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)rebar_attr_set_visualize);

    class_register(CLASS_BOX, c);
    rebar_class = c;

    notify_main(NULL);
    buildspans_main(NULL);
    crucible_main(NULL);
}

void *rebar_new(t_symbol *s, long argc, t_atom *argv) {
    t_rebar *x = (t_rebar *)object_alloc(rebar_class);
    if (x) {
        x->log = 0;
        x->defer = 0;
        x->consume = 0;
        x->visualize = 0;
        x->user_dict_name = gensym("");
        x->tmp_dict_name = gensym("");

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->user_dict_name = atom_getsym(argv);
            char tmp_name[256];
            snprintf(tmp_name, 256, "_rebar_tmp_%p", x);
            x->tmp_dict_name = gensym(tmp_name);
            t_dictionary *d = dictionary_new();
            dictobj_register(d, &x->tmp_dict_name);
            object_release((t_object *)d);
        }

        attr_args_process(x, argc, argv);

        x->out_log = NULL;
        if (x->log) x->out_log = sdk_outlet_new((t_object *)x, NULL);
        x->out_reach = sdk_intout((t_object *)x);
        x->out_fill = sdk_bangout((t_object *)x);
        x->out_data = sdk_listout((t_object *)x);

        critical_enter(g_rebar_crit);
        g_instantiating_rebar = x;

        t_atom args[1];
        atom_setsym(args, x->tmp_dict_name);
        g_current_mod_hint = (int)MOD_NOTIFY;
        x->notify_inst = (struct _notify *)rebar_notify_new(gensym("notify"), 1, args);
        register_module(x->notify_inst, x);

        g_current_mod_hint = (int)MOD_BUILDSPANS;
        x->buildspans_inst = (struct _buildspans *)rebar_buildspans_new(gensym("buildspans"), 0, NULL);
        register_module(x->buildspans_inst, x);

        g_current_mod_hint = (int)MOD_CRUCIBLE;
        x->crucible_inst = (struct _crucible *)rebar_crucible_new(gensym("crucible"), 1, args);
        register_module(x->crucible_inst, x);

        g_instantiating_rebar = NULL;
        critical_exit(g_rebar_crit);

        x->notify_inst->log = x->log;
        x->notify_inst->defer = x->defer;
        x->buildspans_inst->log = x->log;
        x->buildspans_inst->defer = x->defer;
        x->buildspans_inst->visualize = x->visualize;
        x->crucible_inst->log = x->log;
        x->crucible_inst->defer = x->defer;
        x->crucible_inst->consume = x->consume;
    }
    return x;
}

void rebar_free(t_rebar *x) {
    if (x->notify_inst) { unregister_module(x->notify_inst); unregister_outlets(x->notify_inst); rebar_notify_free(x->notify_inst); }
    if (x->buildspans_inst) { unregister_module(x->buildspans_inst); unregister_outlets(x->buildspans_inst); rebar_buildspans_free(x->buildspans_inst); }
    if (x->crucible_inst) { unregister_module(x->crucible_inst); unregister_outlets(x->crucible_inst); rebar_crucible_free(x->crucible_inst); }

    t_dictionary *d = dictobj_findregistered_retain(x->tmp_dict_name);
    if (d) { dictobj_unregister(d); object_release((t_object *)d); }
}

void rebar_request_copy_back(t_rebar *x) {
    if (x->defer) defer_low(x, (method)rebar_do_copy_back, NULL, 0, NULL);
    else rebar_do_copy_back(x);
}

void rebar_do_copy_back(t_rebar *x) {
    t_dictionary *user_dict = dictobj_findregistered_retain(x->user_dict_name);
    t_dictionary *tmp_dict = dictobj_findregistered_retain(x->tmp_dict_name);
    if (user_dict && tmp_dict) {
        dictionary_clear(user_dict);
        dictionary_copyentries(tmp_dict, user_dict, NULL);
    }
    if (user_dict) object_release((t_object *)user_dict);
    if (tmp_dict) object_release((t_object *)tmp_dict);
}

void rebar_int(t_rebar *x, long n) {
    t_dictionary *user_dict = dictobj_findregistered_retain(x->user_dict_name);
    t_dictionary *tmp_dict = dictobj_findregistered_retain(x->tmp_dict_name);
    if (user_dict && tmp_dict) {
        dictionary_clear(tmp_dict);
        dictionary_copyentries(user_dict, tmp_dict, NULL);
    }
    if (user_dict) object_release((t_object *)user_dict);
    if (tmp_dict) object_release((t_object *)tmp_dict);

    rebar_buildspans_local_bar_length(x->buildspans_inst, (double)n);
    rebar_crucible_local_bar_length(x->crucible_inst, (double)n);
    rebar_notify_bang(x->notify_inst);
}

void rebar_assist(t_rebar *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) sprintf(s, "Inlet 1: (int) Trigger isolated coordinated dump with specified bar length.");
    else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1: Data List [palette, track, bar, offset] and Reach Lists from Crucible"); break;
            case 1: sprintf(s, "Outlet 2: Fill bang from Crucible"); break;
            case 2: sprintf(s, "Outlet 3: Reach (int) from Crucible"); break;
            case 3: sprintf(s, "Outlet 4: Logging and Status messages"); break;
        }
    }
}

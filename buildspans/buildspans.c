#include "buildspans.h"
#include "../crucible/crucible.h"
#include "ext_proto.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/visualize.h"
#include "../shared/async_worker.h"
#include "../shared/logging.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// --- HIERARCHICAL DICTIONARY HELPERS ---

t_dictionary* get_sub_dict(t_dictionary *parent, t_symbol *key, int create) {
    if (!parent || !key || key == _sym_nothing) return NULL;
    t_dictionary *sub = NULL;
    t_object *obj = NULL;
    if (dictionary_getobject(parent, key, &obj) == MAX_ERR_NONE && obj) {
        sub = (t_dictionary *)obj;
    } else if (create) {
        sub = dictionary_new();
        dictionary_appenddictionary(parent, key, (t_object *)sub);
    }
    return sub;
}

t_dictionary* buildspans_get_bar_dict(t_buildspans *x, t_symbol *pal, t_symbol *track, t_symbol *bar, int create) {
    t_dictionary *pd = get_sub_dict(x->building, pal, create);
    t_dictionary *td = get_sub_dict(pd, track, create);
    return get_sub_dict(td, bar, create);
}

// Thread-safe selector parsing using stack buffers
int parse_selector_safe(const char *sel, t_symbol **track, t_symbol **bar, t_symbol **prop) {
    char buf[256];
    if (strlen(sel) >= 256) return 0;
    strcpy(buf, sel);
    char *d1 = strstr(buf, "::"); if (!d1) return 0;
    *d1 = '\0'; *track = gensym(buf);
    char *d2 = strstr(d1 + 2, "::"); if (!d2) return 0;
    *d2 = '\0'; *bar = gensym(d1 + 2);
    *prop = gensym(d2 + 2);
    return 1;
}

// --- ASYNC HELPERS ---

void buildspans_offset_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) {
    if (argc >= 2) buildspans_do_offset(x, atom_getfloat(argv), atom_getfloat(argv + 1));
    else if (argc > 0) buildspans_do_offset(x, atom_getfloat(argv), 0.0);
}

void buildspans_track_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) {
    if (argc > 0) buildspans_do_track(x, atom_getlong(argv));
}

void buildspans_list_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) {
    buildspans_do_list(x, s, (long)argc, argv);
}

// --- CORE IMPLEMENTATION ---

long buildspans_get_bar_length(t_buildspans *x) {
    if (x->local_bar_length > 0) return (long)x->local_bar_length;
    if (!x->buffer_ref) return -1;
    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) return -1;
    float *s = buffer_locksamples(b); long bl = (s && buffer_getframecount(b) > 0) ? (long)s[0] : -1; buffer_unlocksamples(b);
    if (bl > 0) x->local_bar_length = (double)bl;
    return bl;
}

void buildspans_do_offset(t_buildspans *x, double f, double loop_start) {
    if (x->async && x->worker && !systhread_ismainthread() && !async_worker_is_worker_thread(x->worker)) {
        t_atom a[2]; atom_setfloat(&a[0], f); atom_setfloat(&a[1], loop_start);
        async_worker_enqueue(x->worker, x, (method)buildspans_offset_deferred, NULL, 2, a);
        return;
    }

    long bl = buildspans_get_bar_length(x);
    if (bl <= 0 || f <= 0.0) { x->current_offset = f; x->loop_start = loop_start; return; }

    long new_off = (long)round(f);
    long old_off = (long)round(x->current_offset);
    if (new_off == old_off) { x->current_offset = f; x->loop_start = loop_start; return; }

    t_dictionary *pd = get_sub_dict(x->building, x->current_palette, 0);
    if (pd) {
        long nt = 0; t_symbol **tk = NULL;
        dictionary_getkeys(pd, &nt, &tk);
        char old_s[32], new_s[32]; snprintf(old_s, 32, "-%ld", old_off); snprintf(new_s, 32, "-%ld", new_off);
        for (long i = 0; i < nt; i++) {
            const char *n = tk[i]->s_name;
            const char *p = strstr(n, old_s);
            if (p && strlen(p) == strlen(old_s)) {
                char target[128]; strncpy(target, n, p - n); target[p - n] = '\0'; strcat(target, new_s);
                t_symbol *ts = gensym(target);
                if (!dictionary_hasentry(pd, ts)) {
                    t_dictionary *std = NULL; dictionary_getdictionary(pd, tk[i], (t_object **)&std);
                    if (std) {
                        t_dictionary *ttd = dictionary_clone(std);
                        long nb = 0; t_symbol **bk = NULL; dictionary_getkeys(ttd, &nb, &bk);
                        for (long j = 0; j < nb; j++) {
                            t_dictionary *bd = NULL; dictionary_getdictionary(ttd, bk[j], (t_object **)&bd);
                            if (bd) {
                                dictionary_appendfloat(bd, gensym("offset"), f);
                                t_atom *abs = NULL; long nabs = 0;
                                dictionary_getatoms(bd, gensym("absolutes"), &nabs, &abs);
                                if (nabs > 0 && abs) {
                                    for (long k = 0; k < nabs; k++) atom_setfloat(abs + k, atom_getfloat(abs + k) + (f - x->current_offset));
                                    dictionary_appendatoms(bd, gensym("absolutes"), nabs, abs);
                                }
                            }
                        }
                        if (bk) sysmem_freeptr(bk);
                        dictionary_appenddictionary(pd, ts, (t_object *)ttd);
                    }
                }
            }
        }
        if (tk) sysmem_freeptr(tk);
    }
    x->current_offset = f; x->loop_start = loop_start;
    if (x->visualize) { char m[256]; snprintf(m, 256, "{\"event\":\"offset\",\"offset\":%f}", f); visualize((t_object *)x, m); }
}

void buildspans_do_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->async && x->worker && !systhread_ismainthread() && !async_worker_is_worker_thread(x->worker)) {
        async_worker_enqueue(x->worker, x, (method)buildspans_list_deferred, s, argc, argv);
        return;
    }

    long bl = buildspans_get_bar_length(x); if (bl <= 0) return;
    double calc, store, score;
    if (argc == 2) { calc = atom_getfloat(argv); score = atom_getfloat(argv + 1); store = calc; }
    else if (argc == 3) { calc = atom_getfloat(argv); score = atom_getfloat(argv + 1); store = atom_getfloat(argv + 2); }
    else return;

    double off = (x->current_offset <= 0.0) ? calc : x->current_offset;
    char tstr[64]; snprintf(tstr, 64, "%ld-%ld", x->current_track, (long)round(off));
    t_symbol *ts = gensym(tstr);
    t_dictionary *td = buildspans_get_bar_dict(x, x->current_palette, ts, _sym_nothing, 1); // Get track dict

    // Resolution: If track exists, use its high-precision offset from the first bar.
    long nb = 0; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk);
    if (nb > 0 && bk) {
        t_dictionary *ebd = NULL; dictionary_getdictionary(td, bk[0], (t_object **)&ebd);
        if (ebd) dictionary_getfloat(ebd, gensym("offset"), &off);
    }
    if (bk) sysmem_freeptr(bk);

    long bts = (long)floor((calc - off + x->loop_start) / bl) * bl;
    char bstr[32]; snprintf(bstr, 32, "%ld", bts);
    t_dictionary *bd = get_sub_dict(td, gensym(bstr), 1);

    dictionary_appendfloat(bd, gensym("offset"), off);
    dictionary_appendsym(bd, gensym("palette"), x->current_palette);

    t_atomarray *aa = NULL;
    if (dictionary_getatomarray(bd, gensym("absolutes"), (t_object **)&aa) != MAX_ERR_NONE || !aa) {
        aa = atomarray_new(0, NULL); t_atom a; atom_setobj(&a, (t_object *)aa); dictionary_appendatom(bd, gensym("absolutes"), &a);
    }
    t_atom nabs; atom_setfloat(&nabs, store); atomarray_appendatom(aa, &nabs);

    if (dictionary_getatomarray(bd, gensym("scores"), (t_object **)&aa) != MAX_ERR_NONE || !aa) {
        aa = atomarray_new(0, NULL); t_atom a; atom_setobj(&a, (t_object *)aa); dictionary_appendatom(bd, gensym("scores"), &a);
    }
    t_atom nsc; atom_setfloat(&nsc, score); atomarray_appendatom(aa, &nsc);

    long n = 0; t_atom *at = NULL; atomarray_getatoms(aa, &n, &at);
    double sum = 0; for (long i = 0; i < n; i++) sum += atom_getfloat(at + i);
    dictionary_appendfloat(bd, gensym("mean"), sum / n);
}

void buildspans_do_track(t_buildspans *x, long n) {
    if (x->async && x->worker && !systhread_ismainthread() && !async_worker_is_worker_thread(x->worker)) {
        t_atom a; atom_setlong(&a, n);
        async_worker_enqueue(x->worker, x, (method)buildspans_track_deferred, NULL, 1, &a);
        return;
    }
    x->current_track = n;
}

void buildspans_do_bang(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->async && x->worker && !systhread_ismainthread() && !async_worker_is_worker_thread(x->worker)) {
        async_worker_enqueue(x->worker, x, (method)buildspans_do_bang, s, argc, argv);
        return;
    }
    dictionary_clear(x->building);
    x->local_bar_length = 0;
}

void buildspans_do_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv, long inlet_num) {
    if (inlet_num == 3) x->current_palette = s;
}

// --- WRAPPERS ---

void buildspans_offset(t_buildspans *x, double f) { buildspans_do_offset(x, f, 0.0); }
void buildspans_track(t_buildspans *x, long n) { buildspans_do_track(x, n); }
void buildspans_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { buildspans_do_list(x, s, argc, argv); }
void buildspans_float(t_buildspans *x, double f) {
    long il = proxy_getinlet((t_object *)x);
    if (il == 1) buildspans_do_offset(x, f, 0.0);
    else if (il == 2) buildspans_do_track(x, (long)f);
    else if (il == 4) x->local_bar_length = f;
}
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
    buildspans_do_anything(x, s, argc, argv, proxy_getinlet((t_object *)x));
}
void buildspans_bang(t_buildspans *x) { buildspans_do_bang(x, NULL, 0, NULL); }
void buildspans_clear(t_buildspans *x) { buildspans_do_bang(x, NULL, 0, NULL); }

void buildspans_set_bar_buffer(t_buildspans *x, t_symbol *s) {
    x->s_buffer_name = s;
    if (x->buffer_ref) buffer_ref_set(x->buffer_ref, s);
    else x->buffer_ref = buffer_ref_new((t_object *)x, s);
}

void buildspans_local_bar_length(t_buildspans *x, double f) { x->local_bar_length = f; }
void buildspans_do_local_bar_length(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { if (argc > 0) x->local_bar_length = atom_getfloat(argv); }

t_max_err buildspans_attr_set_async(t_buildspans *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->async = atom_getlong(av);
        if (x->async && !x->worker) x->worker = async_worker_create();
        else if (!x->async && x->worker) { async_worker_release(x->worker); x->worker = NULL; }
    }
    return MAX_ERR_NONE;
}

t_max_err buildspans_attr_set_log(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) x->log = atom_getlong(av); return MAX_ERR_NONE; }
t_max_err buildspans_attr_set_visualize(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) x->visualize = atom_getlong(av); return MAX_ERR_NONE; }
t_max_err buildspans_attr_set_bind(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) x->bind_name = atom_getsym(av); return MAX_ERR_NONE; }

void buildspans_free(t_buildspans *x) {
    if (x->worker) async_worker_release(x->worker);
    if (x->building) object_free(x->building);
    if (x->tracks_ended_in_current_event) object_free(x->tracks_ended_in_current_event);
    if (x->buffer_ref) object_free(x->buffer_ref);
    if (x->bind_clock) object_free(x->bind_clock);
}

void buildspans_assist(t_buildspans *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) sprintf(s, "Inlet %ld", a);
    else sprintf(s, "Outlet %ld", a);
}

void buildspans_notify(t_buildspans *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {}

t_class *buildspans_class = NULL;

void *buildspans_new(t_symbol *s, long argc, t_atom *argv) {
    t_buildspans *x = (t_buildspans *)object_alloc(buildspans_class);
    x->building = dictionary_new();
    x->tracks_ended_in_current_event = dictionary_new();
    x->current_track = 0; x->current_offset = 0.0; x->loop_start = 0.0; x->current_palette = gensym("default");
    x->async = 0; x->worker = NULL; x->local_bar_length = 0; x->buffer_ref = NULL; x->s_buffer_name = gensym("bar");
    x->bind_name = _sym_nothing; x->bound_crucible = NULL; x->bind_clock = clock_new(x, (method)buildspans_notify);

    attr_args_process(x, argc, argv);
    buildspans_set_bar_buffer(x, x->s_buffer_name);

    floatin((t_object *)x, 4); proxy_new((t_object *)x, 3, NULL); intin((t_object *)x, 2); proxy_new((t_object *)x, 1, NULL);
    x->log_outlet = outlet_new((t_object *)x, NULL);
    x->out_bar_data = outlet_new((t_object *)x, NULL);
    x->track_outlet = outlet_new((t_object *)x, NULL);
    x->span_outlet = outlet_new((t_object *)x, NULL);
    return x;
}

#ifndef NO_EXT_MAIN
void ext_main(void *r) {
    t_class *c = class_new("buildspans", (method)buildspans_new, (method)buildspans_free, sizeof(t_buildspans), 0L, A_GIMME, 0);
    class_addmethod(c, (method)buildspans_clear, "clear", 0);
    class_addmethod(c, (method)buildspans_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_track, "in2", A_LONG, 0);
    class_addmethod(c, (method)buildspans_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_bang, "bang", 0);
    CLASS_ATTR_LONG(c, "async", 0, t_buildspans, async); CLASS_ATTR_ACCESSORS(c, "async", NULL, (method)buildspans_attr_set_async);
    CLASS_ATTR_LONG(c, "visualize", 0, t_buildspans, visualize); CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)buildspans_attr_set_visualize);
    CLASS_ATTR_SYM(c, "bind", 0, t_buildspans, bind_name); CLASS_ATTR_ACCESSORS(c, "bind", NULL, (method)buildspans_attr_set_bind);
    class_register(CLASS_BOX, c); buildspans_class = c;
}
#endif

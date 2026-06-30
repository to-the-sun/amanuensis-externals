#include "crucible.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include "../shared/visualize.h"
#include "../shared/async_worker.h"
#include <string.h>
#include <ctype.h>

// Prototypes
void *crucible_new(t_symbol *s, long argc, t_atom *argv);
void crucible_free(t_crucible *x);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_log(t_crucible *x, const char *fmt, ...);
char *crucible_atoms_to_string(long argc, t_atom *argv);
int parse_selector(const char *selector_str, t_symbol **track, t_symbol **bar, t_symbol **key);
t_dictionary *dictionary_deep_copy(t_dictionary *src);
void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, t_atom_long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict);
void crucible_local_bar_length(t_crucible *x, double f);
void crucible_do_local_bar_length(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
t_max_err crucible_attr_set_log(t_crucible *x, void *attr, long ac, t_atom *av);
t_max_err crucible_attr_set_async(t_crucible *x, void *attr, long ac, t_atom *av);
t_max_err crucible_attr_set_consume(t_crucible *x, void *attr, long ac, t_atom *av);
t_atom_long crucible_get_bar_length(t_crucible *x);
t_atomarray *crucible_get_span_as_atomarray(t_dictionary *bar_dict);
int crucible_span_has_loser(t_atomarray *span_aa, t_dictionary *defeated_dict);
void crucible_recalculate_reaches(t_crucible *x);
void crucible_visualize_dump_all_spans(t_crucible *x);
void crucible_visualize_state(t_crucible *x, t_symbol *event_type, t_symbol *track_id_sym, t_atomarray *span_aa, double rating, int include_tracks);
t_max_err crucible_attr_set_visualize(t_crucible *x, void *attr, long ac, t_atom *av);

t_class *crucible_class;

t_max_err dictionary_getatoms_robust(t_dictionary *d, t_symbol *k, long *ac, t_atom **av, t_atomarray **aa_out) {
    t_atomarray *aa = NULL; t_atom a;
    if (dictionary_getatomarray(d, k, (t_object **)&aa) == MAX_ERR_NONE && aa) {
        atomarray_getatoms(aa, ac, av);
        if (aa_out) *aa_out = aa; return MAX_ERR_NONE;
    } else if (dictionary_getatom(d, k, &a) == MAX_ERR_NONE) {
        *ac = 1; *av = (t_atom *)sysmem_newptr(sizeof(t_atom));
        if (*av) { **av = a; if (aa_out) *aa_out = NULL; return MAX_ERR_NONE; }
    }
    return MAX_ERR_GENERIC;
}

void crucible_defer_output(t_crucible *x, t_symbol *s, short argc, t_atom *argv) {
    if (s == gensym("-")) outlet_anything(x->outlet_data, s, argc, argv);
    else if (s == gensym("data_list")) outlet_list(x->outlet_data, NULL, argc, argv);
    else if (s == gensym("reach_song")) outlet_anything(x->outlet_reach_int, gensym("song"), argc, argv);
    else if (s == gensym("reach_list")) outlet_list(x->outlet_reach_int, NULL, argc, argv);
    else if (s == gensym("fill")) outlet_anything(x->outlet_fill, gensym("fill"), 0, NULL);
}

void crucible_log(t_crucible *x, const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "crucible", fmt, args);
    va_end(args);
}

char *crucible_atoms_to_string(long argc, t_atom *argv) {
    if (argc == 0 || !argv) { char *e = (char *)sysmem_newptr(3); strcpy(e, "[]"); return e; }
    long sz = 256; char *buf = (char *)sysmem_newptr(sz); long off = 0;
    off += snprintf(buf + off, sz - off, "[");
    for (long i = 0; i < argc; i++) {
        char t[128];
        if (atom_gettype(argv + i) == A_FLOAT) snprintf(t, 128, "%.2f", atom_getfloat(argv + i));
        else if (atom_gettype(argv + i) == A_LONG) snprintf(t, 128, "%lld", (long long)atom_getlong(argv + i));
        else if (atom_gettype(argv + i) == A_SYM) snprintf(t, 128, "%s", atom_getsym(argv + i)->s_name);
        else snprintf(t, 128, "?");
        if (sz - off < (long)strlen(t) + 4) { sz *= 2; buf = (char *)sysmem_resizeptr(buf, sz); if (!buf) return NULL; }
        off += snprintf(buf + off, sz - off, "%s", t);
        if (i < argc - 1) off += snprintf(buf + off, sz - off, ", ");
    }
    snprintf(buf + off, sz - off, "]"); return buf;
}

int parse_selector(const char *selector_str, t_symbol **track, t_symbol **bar, t_symbol **key) {
    char buf[512]; strncpy(buf, selector_str, 512); buf[511] = '\0';
    char *f = strstr(buf, "::"); if (!f) return 0; *f = '\0'; *track = gensym(buf);
    char *s = strstr(f + 2, "::"); if (!s) return 0; *s = '\0'; *bar = gensym(f + 2);
    *key = gensym(s + 2); return 1;
}

static void crucible_main_hidden(void *r) {
    common_symbols_init(); t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, sizeof(t_crucible), 0L, A_GIMME, 0);
    class_addmethod(c, (method)crucible_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)crucible_local_bar_length, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)crucible_assist, "assist", A_CANT, 0);
    CLASS_ATTR_LONG(c, "log", 0, t_crucible, log); CLASS_ATTR_ACCESSORS(c, "log", NULL, (method)crucible_attr_set_log);
    CLASS_ATTR_LONG(c, "consume", 0, t_crucible, consume); CLASS_ATTR_ACCESSORS(c, "consume", NULL, (method)crucible_attr_set_consume);
    CLASS_ATTR_LONG(c, "defer", 0, t_crucible, defer);
    CLASS_ATTR_LONG(c, "async", 0, t_crucible, async); CLASS_ATTR_ACCESSORS(c, "async", NULL, (method)crucible_attr_set_async);
    CLASS_ATTR_LONG(c, "visualize", 0, t_crucible, visualize); CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)crucible_attr_set_visualize);
    class_register(CLASS_BOX, c); crucible_class = c;
}

#ifndef NO_EXT_MAIN
void ext_main(void *r) { crucible_main_hidden(r); }
#endif

void *crucible_new(t_symbol *s, long argc, t_atom *argv) {
    t_crucible *x = (t_crucible *)object_alloc(crucible_class);
    if (x) {
        visualize_init(); x->challenger_dict = dictionary_new(); x->last_track_id = gensym("");
        x->incumbent_dict_name = gensym(""); x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        x->log = 0; x->consume = 0; x->defer = 0; x->async = 0; x->worker = NULL; x->visualize = 0;
        x->song_reach = 0; x->track_reaches_dict = dictionary_new(); x->local_bar_length = 0;
        x->instance_id = 1000 + (rand() % 9000); x->bar_warn_sent = 0;
        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->incumbent_dict_name = atom_getsym(argv); argc--; argv++;
        }
        attr_args_process(x, argc, argv);
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->outlet_reach_int = outlet_new((t_object *)x, NULL);
        x->outlet_fill = outlet_new((t_object *)x, NULL);
        x->outlet_data = outlet_new((t_object *)x, NULL);
        floatin((t_object *)x, 1);
    }
    return (x);
}

void crucible_free(t_crucible *x) {
    visualize_cleanup(); if (x->worker) async_worker_release(x->worker);
    if (x->challenger_dict) object_release((t_object *)x->challenger_dict);
    if (x->track_reaches_dict) object_release((t_object *)x->track_reaches_dict);
    if (x->buffer_ref) object_free(x->buffer_ref);
}

void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, t_atom_long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict) {
    if (!bar_dict) return;
    t_atom *span_atoms = NULL; long span_len = 0; t_atomarray *span_aa = NULL;
    if (dictionary_getatoms_robust(bar_dict, gensym("span"), &span_len, &span_atoms, &span_aa) == MAX_ERR_NONE) {
        if (span_len > 0) {
            t_atom_long mv = 0; for (long j = 0; j < span_len; j++) { t_atom_long v = atom_getlong(span_atoms + j); if (v > mv) mv = v; }
            t_atom_long r = mv + crucible_get_bar_length(x);
            char rs[64]; snprintf(rs, 64, "%lld", (long long)r);
            if (incumbent_track_dict && !dictionary_hasentry(incumbent_track_dict, gensym(rs))) {
                t_atom rl[3]; atom_setlong(rl, (t_atom_long)atol(track_sym->s_name)); atom_setlong(rl + 1, r); atom_setfloat(rl + 2, -999999.0);
                if (x->outlet_data) { if (!x->async || systhread_ismainthread()) outlet_anything(x->outlet_data, gensym("-"), 3, rl); else defer(x, (method)crucible_defer_output, gensym("-"), 3, rl); }
            }
        }
        if (!span_aa) sysmem_freeptr(span_atoms);
    }
    t_atom list[4]; t_symbol *pal = _sym_nothing; double off = 0.0;
    t_atom *pav = NULL; long pac = 0; t_atomarray *paa = NULL;
    if (dictionary_getatoms_robust(bar_dict, gensym("palette"), &pac, &pav, &paa) == MAX_ERR_NONE) { if (pac > 0) pal = atom_getsym(pav); if (!paa) sysmem_freeptr(pav); }
    t_atom *oav = NULL; long oac = 0; t_atomarray *oaa = NULL;
    if (dictionary_getatoms_robust(bar_dict, gensym("offset"), &oac, &oav, &oaa) == MAX_ERR_NONE) { if (oac > 0) off = atom_getfloat(oav); if (!oaa) sysmem_freeptr(oav); }
    atom_setsym(list, pal); atom_setlong(list + 1, (t_atom_long)atol(track_sym->s_name)); atom_setlong(list + 2, bar_ts_long); atom_setfloat(list + 3, off);
    if (x->outlet_data) { if (!x->async || systhread_ismainthread()) outlet_list(x->outlet_data, NULL, 4, list); else defer(x, (method)crucible_defer_output, gensym("data_list"), 4, list); }
}

void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray) {
    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name); if (!incumbent_dict) return;
    long span_len = 0; t_atom *span_atoms = NULL; atomarray_getatoms(span_atomarray, &span_len, &span_atoms);
    int challenger_wins = 1; double challenger_winning_rating = 0.0;
    t_dictionary *challenger_track_dict = NULL; dictionary_getdictionary(x->challenger_dict, track_sym, (t_object **)&challenger_track_dict);
    if (!challenger_track_dict) goto cleanup;
    for (long i = 0; i < span_len; i++) {
        t_atom_long bar_ts = atom_getlong(span_atoms + i); char bar_str[64]; snprintf(bar_str, 64, "%lld", (long long)bar_ts);
        t_dictionary *cbd = NULL; dictionary_getdictionary(challenger_track_dict, gensym(bar_str), (t_object **)&cbd); if (!cbd) continue;
        t_atom *rca = NULL; long rcl = 0; t_atomarray *rcaa = NULL;
        if (dictionary_getatoms_robust(cbd, gensym("rating"), &rcl, &rca, &rcaa) == MAX_ERR_NONE) {
            double cr = atom_getfloat(rca); if (i == 0) challenger_winning_rating = cr;
            t_dictionary *it = NULL; if (dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&it) == MAX_ERR_NONE && it) {
                t_dictionary *ib = NULL; if (dictionary_getdictionary(it, gensym(bar_str), (t_object **)&ib) == MAX_ERR_NONE && ib) {
                    t_atom *ria = NULL; long ril = 0; t_atomarray *riaa = NULL;
                    if (dictionary_getatoms_robust(ib, gensym("rating"), &ril, &ria, &riaa) == MAX_ERR_NONE) {
                        if (cr <= atom_getfloat(ria)) challenger_wins = 0; if (!riaa) sysmem_freeptr(ria);
                    }
                }
            }
            if (!rcaa) sysmem_freeptr(rca);
        }
        if (!challenger_wins) break;
    }
    t_dictionary *incumbent_track_dict = NULL, *defeated_dict = NULL, *challenger_span_ts_dict = NULL;
    t_atom_long max_reach = 0; int track_grew = 0, song_grew = 0;
    if (challenger_wins) {
        defeated_dict = dictionary_new(); challenger_span_ts_dict = dictionary_new();
        for (long i = 0; i < span_len; i++) {
            t_atom_long btl = atom_getlong(span_atoms + i); char bts[64]; snprintf(bts, 64, "%lld", (long long)btl); t_symbol *bs = gensym(bts);
            dictionary_appendlong(challenger_span_ts_dict, bs, 1);
            if (dictionary_hasentry(incumbent_dict, track_sym)) {
                t_dictionary *itt = NULL; dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&itt);
                if (itt && dictionary_hasentry(itt, bs)) dictionary_appendlong(defeated_dict, bs, 1);
            }
            t_dictionary *cbd = NULL; dictionary_getdictionary(challenger_track_dict, bs, (t_object **)&cbd); if (!cbd) continue;
            t_atomarray *bsaa = crucible_get_span_as_atomarray(cbd);
            if (bsaa) {
                long bsl; t_atom *bsat; atomarray_getatoms(bsaa, &bsl, &bsat);
                t_atom_long lmv = 0; for (long j = 0; j < bsl; j++) { t_atom_long tv = atom_getlong(bsat + j); if (tv > lmv) lmv = tv; }
                t_atom_long cr = lmv + crucible_get_bar_length(x); if (cr > max_reach) max_reach = cr;
                object_release((t_object *)bsaa);
            }
        }
        t_atom_long ctr = 0; dictionary_getlong(x->track_reaches_dict, track_sym, &ctr);
        track_grew = (max_reach > ctr); song_grew = (max_reach > x->song_reach);
        if (track_grew) { dictionary_deleteentry(x->track_reaches_dict, track_sym); dictionary_appendlong(x->track_reaches_dict, track_sym, max_reach); }
        if (song_grew) x->song_reach = max_reach;
        if (!dictionary_hasentry(incumbent_dict, track_sym)) { incumbent_track_dict = dictionary_new(); dictionary_appenddictionary(incumbent_dict, track_sym, (t_object *)incumbent_track_dict); dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict); }
        else dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict);
        if (x->consume && dictionary_getentrycount(defeated_dict) > 0) {
            t_dictionary *td = dictionary_new(); long nk; t_symbol **ks; dictionary_getkeys(defeated_dict, &nk, &ks);
            for (long i = 0; i < nk; i++) {
                t_dictionary *dbd = NULL; dictionary_getdictionary(incumbent_track_dict, ks[i], (t_object **)&dbd);
                if (dbd) {
                    t_atomarray *isa = crucible_get_span_as_atomarray(dbd);
                    if (isa) {
                        long isc; t_atom *isat; atomarray_getatoms(isa, &isc, &isat);
                        for (long j = 0; j < isc; j++) {
                            char tss[64]; snprintf(tss, 64, "%lld", (long long)atom_getlong(isat + j));
                            if (!dictionary_hasentry(challenger_span_ts_dict, gensym(tss))) dictionary_appendlong(td, gensym(tss), 1);
                        }
                        object_release((t_object *)isa);
                    }
                }
            }
            long dnk; t_symbol **dks; dictionary_getkeys(td, &dnk, &dks);
            for (long i = 0; i < dnk; i++) {
                t_dictionary *dbd = NULL; dictionary_getdictionary(incumbent_track_dict, dks[i], (t_object **)&dbd);
                if (dbd) {
                    t_atomarray *dsa = crucible_get_span_as_atomarray(dbd);
                    if (crucible_span_has_loser(dsa, defeated_dict)) dictionary_deleteentry(incumbent_track_dict, dks[i]);
                    if (dsa) object_release((t_object *)dsa);
                }
            }
            if (dks) sysmem_freeptr(dks); if (ks) sysmem_freeptr(ks); object_release((t_object *)td);
        }
        for (long i = 0; i < span_len; i++) {
            t_atom_long btl = atom_getlong(span_atoms + i); char bts[64]; snprintf(bts, 64, "%lld", (long long)btl); t_symbol *bs = gensym(bts);
            t_dictionary *cbd = NULL; dictionary_getdictionary(challenger_track_dict, bs, (t_object **)&cbd);
            if (cbd) { dictionary_deleteentry(incumbent_track_dict, bs); dictionary_appenddictionary(incumbent_track_dict, bs, (t_object *)dictionary_deep_copy(cbd)); }
        }
        if (x->visualize) crucible_visualize_state(x, gensym("new_span"), track_sym, span_atomarray, challenger_winning_rating, 0);
    }
    dictionary_deleteentry(x->challenger_dict, track_sym);
    if (challenger_wins && incumbent_track_dict) {
        if (song_grew || track_grew) {
            if (x->outlet_reach_int) {
                if (song_grew) { t_atom ra; atom_setlong(&ra, x->song_reach); if (!x->async || systhread_ismainthread()) outlet_anything(x->outlet_reach_int, gensym("song"), 1, &ra); else defer(x, (method)crucible_defer_output, gensym("reach_song"), 1, &ra); }
                if (track_grew) { t_atom rl[2]; atom_setlong(rl, (t_atom_long)atol(track_sym->s_name)); atom_setlong(rl + 1, max_reach); if (!x->async || systhread_ismainthread()) outlet_list(x->outlet_reach_int, NULL, 2, rl); else defer(x, (method)crucible_defer_output, gensym("reach_list"), 2, rl); }
            }
            if (song_grew && x->outlet_fill) { if (!x->async || systhread_ismainthread()) outlet_anything(x->outlet_fill, gensym("fill"), 0, NULL); else defer(x, (method)crucible_defer_output, gensym("fill"), 0, NULL); }
        }
        for (long i = 0; i < span_len; i++) {
            t_atom_long btl = atom_getlong(span_atoms + i); char bts[64]; snprintf(bts, 64, "%lld", (long long)btl);
            t_dictionary *bd = NULL; dictionary_getdictionary(incumbent_track_dict, gensym(bts), (t_object **)&bd);
            if (bd) crucible_output_bar_data(x, bd, btl, track_sym, incumbent_track_dict);
        }
    }
cleanup:
    if (defeated_dict) object_release((t_object *)defeated_dict);
    if (challenger_span_ts_dict) object_release((t_object *)challenger_span_ts_dict);
    if (incumbent_dict) object_release((t_object *)incumbent_dict);
}

t_atom_long crucible_get_bar_length(t_crucible *x) {
    if (x->local_bar_length > 0) return (t_atom_long)x->local_bar_length;
    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) { if (!x->bar_warn_sent) { object_warn((t_object *)x, "bar buffer~ not found"); x->bar_warn_sent = 1; } buffer_ref_set(x->buffer_ref, _sym_nothing); buffer_ref_set(x->buffer_ref, gensym("bar")); b = buffer_ref_getobject(x->buffer_ref); }
    if (!b) return 0; x->bar_warn_sent = 0; t_atom_long bl = 0;
    critical_enter(0); float *s = buffer_locksamples(b); if (s) { if (buffer_getframecount(b) > 0) bl = (t_atom_long)s[0]; buffer_unlocksamples(b); }
    critical_exit(0); if (bl > 0) x->local_bar_length = (double)bl; return bl;
}

void crucible_local_bar_length(t_crucible *x, double f) {
    if (x->async && x->worker && !systhread_ismainthread()) { t_atom a; atom_setfloat(&a, f); async_worker_enqueue(x->worker, x, (method)crucible_do_local_bar_length, NULL, 1, &a); return; }
    t_atom a; atom_setfloat(&a, f); crucible_do_local_bar_length(x, NULL, 1, &a);
}

void crucible_do_local_bar_length(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    double f = atom_getfloat(argv); if (f <= 0) x->local_bar_length = 0; else x->local_bar_length = f;
}

t_max_err crucible_attr_set_async(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) { x->async = atom_getlong(av); if (x->async && !x->worker) x->worker = async_worker_create(); else if (!x->async && x->worker) { async_worker_release(x->worker); x->worker = NULL; } }
    return MAX_ERR_NONE;
}

t_max_err crucible_attr_set_log(t_crucible *x, void *attr, long ac, t_atom *av) { if (ac && av) x->log = atom_getlong(av); return MAX_ERR_NONE; }
t_max_err crucible_attr_set_consume(t_crucible *x, void *attr, long ac, t_atom *av) { if (ac && av) x->consume = atom_getlong(av); return MAX_ERR_NONE; }

t_dictionary *dictionary_deep_copy(t_dictionary *src) {
    if (!src) return NULL; t_dictionary *dst = dictionary_new(); long nk = 0; t_symbol **ks = NULL; dictionary_getkeys(src, &nk, &ks);
    for (long i = 0; i < nk; i++) {
        t_atom v; dictionary_getatom(src, ks[i], &v);
        if (atom_gettype(&v) == A_OBJ) {
            t_object *o = atom_getobj(&v);
            if (o) {
                if (object_classname_compare(o, gensym("dictionary"))) { t_dictionary *ns = (t_dictionary *)o; t_dictionary *nd = dictionary_deep_copy(ns); if (nd) dictionary_appenddictionary(dst, ks[i], (t_object *)nd); }
                else if (object_classname_compare(o, gensym("atomarray"))) { t_atomarray *as = (t_atomarray *)o; long al; t_atom *at; atomarray_getatoms(as, &al, &at); t_atomarray *ad = atomarray_new(al, at); if (ad) dictionary_appendatomarray(dst, ks[i], (t_object *)ad); }
            }
        } else dictionary_appendatom(dst, ks[i], &v);
    }
    if (ks) sysmem_freeptr(ks); return dst;
}

t_atomarray *crucible_get_span_as_atomarray(t_dictionary *bar_dict) {
    long ac = 0; t_atom *av = NULL; t_atomarray *aa = NULL;
    if (dictionary_getatoms_robust(bar_dict, gensym("span"), &ac, &av, &aa) == MAX_ERR_NONE) {
        if (aa) { object_retain((t_object *)aa); return aa; }
        else { t_atomarray *new_aa = atomarray_new(1, av); sysmem_freeptr(av); return new_aa; }
    }
    return NULL;
}

int crucible_span_has_loser(t_atomarray *span_aa, t_dictionary *defeated_dict) {
    if (!span_aa || !defeated_dict) return 0;
    long sl = 0; t_atom *sa = NULL; atomarray_getatoms(span_aa, &sl, &sa);
    for (long i = 0; i < sl; i++) {
        char ts[64]; snprintf(ts, 64, "%lld", (long long)atom_getlong(sa + i));
        if (dictionary_hasentry(defeated_dict, gensym(ts))) return 1;
    }
    return 0;
}

void crucible_recalculate_reaches(t_crucible *x) {
    t_atom_long bl = crucible_get_bar_length(x);
    t_dictionary *id = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!id) return;
    x->song_reach = 0; dictionary_clear(x->track_reaches_dict);
    long nt; t_symbol **tk = NULL; dictionary_getkeys(id, &nt, &tk);
    for (long i = 0; i < nt; i++) {
        t_dictionary *td = NULL; dictionary_getdictionary(id, tk[i], (t_object **)&td); if (!td) continue;
        t_atom_long tmr = 0; long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk);
        for (long j = 0; j < nb; j++) {
            t_dictionary *bd = NULL; dictionary_getdictionary(td, bk[j], (t_object **)&bd); if (!bd) continue;
            t_atom_long bts = atoll(bk[j]->s_name); if (bts + bl > tmr) tmr = bts + bl;
            t_atomarray *sa = crucible_get_span_as_atomarray(bd);
            if (sa) {
                long sl; t_atom *sat; atomarray_getatoms(sa, &sl, &sat);
                for (long k = 0; k < sl; k++) { t_atom_long tv = atom_getlong(sat + k); if (tv + bl > tmr) tmr = tv + bl; }
                object_release((t_object *)sa);
            }
        }
        if (bk) sysmem_freeptr(bk); dictionary_appendlong(x->track_reaches_dict, tk[i], tmr);
        if (tmr > x->song_reach) x->song_reach = tmr;
    }
    if (tk) sysmem_freeptr(tk); dictobj_release(id);
}

void crucible_visualize_dump_all_spans(t_crucible *x) {
    if (!x->visualize) return; t_dictionary *id = dictobj_findregistered_retain(x->incumbent_dict_name); if (!id) return;
    long nt; t_symbol **tk = NULL; dictionary_getkeys(id, &nt, &tk);
    for (long i = 0; i < nt; i++) {
        t_dictionary *td = NULL; if (dictionary_getdictionary(id, tk[i], (t_object **)&td) != MAX_ERR_NONE || !td) continue;
        long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk); t_dictionary *ss = dictionary_new();
        for (long j = 0; j < nb; j++) {
            t_dictionary *bd = NULL; dictionary_getdictionary(td, bk[j], (t_object **)&bd); if (!bd) continue;
            t_atomarray *sa = crucible_get_span_as_atomarray(bd); if (!sa) continue;
            long ac; t_atom *av; atomarray_getatoms(sa, &ac, &av);
            if (ac > 0) {
                char fbs[64]; snprintf(fbs, 64, "%lld", (long long)atom_getlong(av));
                if (!dictionary_hasentry(ss, gensym(fbs))) {
                    dictionary_appendlong(ss, gensym(fbs), 1); double r = 0; t_atom ra;
                    if (dictionary_getatom(bd, gensym("rating"), &ra) == MAX_ERR_NONE) r = atom_getfloat(&ra);
                    crucible_visualize_state(x, gensym("new_span"), tk[i], sa, r, 0);
                }
            }
            object_release((t_object *)sa);
        }
        if (bk) sysmem_freeptr(bk); object_release((t_object *)ss);
    }
    if (tk) sysmem_freeptr(tk); dictobj_release(id);
}

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->async && x->worker && !systhread_ismainthread()) { async_worker_enqueue(x->worker, x, (method)crucible_do_anything, s, argc, argv); return; }
    if (x->defer && !systhread_ismainthread()) { defer(x, (method)crucible_anything, s, argc, argv); return; }
    crucible_do_anything(x, s, argc, argv);
}

void crucible_do_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    if (s == gensym("clear")) {
        x->song_reach = 0; if (x->track_reaches_dict) dictionary_clear(x->track_reaches_dict);
        if (x->challenger_dict) dictionary_clear(x->challenger_dict); x->last_track_id = gensym("");
        if (x->visualize) visualize((t_object *)x, "{\"tracks\":{}}"); return;
    }
    if (s == gensym("reaches")) {
        t_symbol *tmp = x->incumbent_dict_name; x->incumbent_dict_name = _sym_nothing; x->incumbent_dict_name = tmp;
        crucible_recalculate_reaches(x); if (x->visualize) { crucible_visualize_state(x, NULL, NULL, NULL, 0.0, 1); crucible_visualize_dump_all_spans(x); }
        if (x->outlet_reach_int) {
            t_atom sra; atom_setlong(&sra, x->song_reach); if (!x->async || systhread_ismainthread()) outlet_anything(x->outlet_reach_int, gensym("song"), 1, &sra); else defer(x, (method)crucible_defer_output, gensym("reach_song"), 1, &sra);
            if (x->track_reaches_dict) {
                t_symbol **ks = NULL; long nk = 0; dictionary_getkeys(x->track_reaches_dict, &nk, &ks);
                for (long i = 0; i < nk; i++) { t_atom_long r = 0; dictionary_getlong(x->track_reaches_dict, ks[i], &r); t_atom rl[2]; atom_setlong(rl, (t_atom_long)atol(ks[i]->s_name)); atom_setlong(rl + 1, r); if (!x->async || systhread_ismainthread()) outlet_list(x->outlet_reach_int, NULL, 2, rl); else defer(x, (method)crucible_defer_output, gensym("reach_list"), 2, rl); }
                if (ks) sysmem_freeptr(ks);
            }
        }
        return;
    }
    if (s == gensym("track") && argc > 0) {
        if (atom_gettype(argv) == A_LONG) { char ids[64]; snprintf(ids, 64, "%lld", (long long)atom_getlong(argv)); x->last_track_id = gensym(ids); }
        else if (atom_gettype(argv) == A_SYM) x->last_track_id = atom_getsym(argv);
        return;
    }
    if (s == gensym("span") && argc > 0) {
        if (x->last_track_id == _sym_nothing || x->last_track_id == gensym("")) return;
        t_atomarray *sa = atomarray_new(argc, argv); crucible_process_span(x, x->last_track_id, sa); object_release((t_object *)sa); return;
    }
    if (s == gensym("replace") && argc >= 2 && x->visualize) {
        t_symbol *ts = NULL, *bs = NULL, *ks = NULL;
        if (atom_gettype(argv) == A_SYM && parse_selector(atom_getsym(argv)->s_name, &ts, &bs, &ks)) {
            if (strcmp(ks->s_name, "rating") == 0) { char m[256]; snprintf(m, 256, "{\"event\":\"replace\",\"track\":\"%s\",\"bar\":\"%s\",\"rating\":%.6f}", ts->s_name, bs->s_name, atom_getfloat(argv + 1)); visualize((t_object *)x, m); }
        }
        return;
    }
    t_symbol *ts = NULL, *bs = NULL, *ks = NULL;
    if (parse_selector(s->s_name, &ts, &bs, &ks)) {
        t_dictionary *td = NULL; if (!dictionary_hasentry(x->challenger_dict, ts)) { td = dictionary_new(); dictionary_appenddictionary(x->challenger_dict, ts, (t_object *)td); } else dictionary_getdictionary(x->challenger_dict, ts, (t_object **)&td);
        t_dictionary *bd = NULL; if (!dictionary_hasentry(td, bs)) { bd = dictionary_new(); dictionary_appenddictionary(td, bs, (t_object *)bd); } else dictionary_getdictionary(td, bs, (t_object **)&bd);
        if (argc == 1) dictionary_appendatom(bd, ks, argv); else dictionary_appendatomarray(bd, ks, (t_object *)atomarray_new(argc, argv));
    }
}

void crucible_assist(t_crucible *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1: Primary messages (clear, track, span, reaches, replace, log, consume, visualize, async). Also sets incumbent dictionary name."); break;
            case 1: sprintf(s, "Inlet 2: Local Bar Length (float)."); break;
        }
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Outlet 1: Data Outlet. Outputs bar data lists '[palette] [track] [bar] [offset]' and reach update notifications '[- track reach -999999.0]'."); break;
            case 1: sprintf(s, "Outlet 2: Fill Outlet. Outputs the symbol 'fill' whenever a song growth event is detected."); break;
            case 2: sprintf(s, "Outlet 3: Reach Outlet. Outputs current reaches: 'song [reach]' or '[track_id] [reach]'. Triggered by growth or 'reaches' message."); break;
            case 3: sprintf(s, "Outlet 4: Logging Outlet. Outputs verbose diagnostic and status messages when the @log attribute is enabled."); break;
        }
    }
}

static long json_append_atom_or_array(char *buffer, long offset, long buffer_size, t_dictionary *dict, t_symbol *key) {
    if (offset >= buffer_size - 1) return offset;
    long ac = 0; t_atom *av = NULL; t_atomarray *aa = NULL;
    if (dictionary_getatoms_robust(dict, key, &ac, &av, &aa) == MAX_ERR_NONE) {
        if (ac > 1 || aa) {
            offset += snprintf(buffer + offset, buffer_size - offset, "[");
            for (long i = 0; i < ac; i++) {
                if (offset >= buffer_size - 1) break;
                if (atom_gettype(av + i) == A_FLOAT) offset += snprintf(buffer + offset, buffer_size - offset, "%.6f", atom_getfloat(av + i));
                else if (atom_gettype(av + i) == A_LONG) offset += snprintf(buffer + offset, buffer_size - offset, "%lld", (long long)atom_getlong(av + i));
                if (i < ac - 1 && offset < buffer_size - 1) offset += snprintf(buffer + offset, buffer_size - offset, ",");
            }
            if (offset < buffer_size - 1) offset += snprintf(buffer + offset, buffer_size - offset, "]");
        } else if (ac == 1) {
            if (atom_gettype(av) == A_FLOAT) offset += snprintf(buffer + offset, buffer_size - offset, "%.6f", atom_getfloat(av));
            else if (atom_gettype(av) == A_LONG) offset += snprintf(buffer + offset, buffer_size - offset, "%lld", (long long)atom_getlong(av));
        }
        if (!aa) sysmem_freeptr(av);
    } else {
        offset += snprintf(buffer + offset, buffer_size - offset, "null");
    }
    return (offset < buffer_size) ? offset : buffer_size - 1;
}

void crucible_visualize_state(t_crucible *x, t_symbol *event_type, t_symbol *track_id_sym, t_atomarray *span_aa, double rating, int include_tracks) {
    if (!x->visualize) return; t_dictionary *id = dictobj_findregistered_retain(x->incumbent_dict_name); if (!id) return;
    long sz = 262144; char *buf = (char *)sysmem_newptr(sz); if (!buf) { dictobj_release(id); return; }
    long off = 0; off += snprintf(buf + off, sz - off, "{\"bar_length\":%lld", (long long)crucible_get_bar_length(x));
    if (event_type && event_type != _sym_nothing) {
        off += snprintf(buf + off, sz - off, ",\"event\":\"%s\"", event_type->s_name);
        if (track_id_sym) off += snprintf(buf + off, sz - off, ",\"new_span_track\":\"%s\"", track_id_sym->s_name);
        if (span_aa) {
            long ac; t_atom *av; atomarray_getatoms(span_aa, &ac, &av); off += snprintf(buf + off, sz - off, ",\"new_span_bars\":[");
            for (long i = 0; i < ac; i++) off += snprintf(buf + off, sz - off, "%lld%s", (long long)atom_getlong(av + i), (i < ac - 1) ? "," : "");
            off += snprintf(buf + off, sz - off, "]");
            t_dictionary *td = NULL; if (dictionary_getdictionary(id, track_id_sym, (t_object **)&td) == MAX_ERR_NONE && td) {
                off += snprintf(buf + off, sz - off, ",\"new_span_data\":{");
                for (long i = 0; i < ac; i++) {
                    t_atom_long bts = atom_getlong(av + i); char btss[64]; snprintf(btss, 64, "%lld", (long long)bts);
                    t_dictionary *bd = NULL; if (dictionary_getdictionary(td, gensym(btss), (t_object **)&bd) == MAX_ERR_NONE && bd) {
                        if (i > 0) off += snprintf(buf + off, sz - off, ","); off += snprintf(buf + off, sz - off, "\"%lld\":{\"absolutes\":", (long long)bts);
                        off = json_append_atom_or_array(buf, off, sz, bd, gensym("absolutes")); off += snprintf(buf + off, sz - off, ",\"scores\":");
                        off = json_append_atom_or_array(buf, off, sz, bd, gensym("scores")); off += snprintf(buf + off, sz - off, ",\"offset\":");
                        off = json_append_atom_or_array(buf, off, sz, bd, gensym("offset")); off += snprintf(buf + off, sz - off, "}");
                    }
                }
                off += snprintf(buf + off, sz - off, "}");
            }
        }
        off += snprintf(buf + off, sz - off, ",\"new_span_rating\":%.4f", rating);
    }
    if (include_tracks) {
        off += snprintf(buf + off, sz - off, ",\"tracks\":{"); long nt; t_symbol **tk = NULL; dictionary_getkeys(id, &nt, &tk);
        for (long i = 0; i < nt; i++) {
            t_dictionary *td = NULL; if (dictionary_getdictionary(id, tk[i], (t_object **)&td) != MAX_ERR_NONE || !td) continue;
            if (i > 0) off += snprintf(buf + off, sz - off, ","); off += snprintf(buf + off, sz - off, "\"%s\":[", tk[i]->s_name);
            long nb; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk);
            for (long j = 0; j < nb; j++) { if (j > 0) off += snprintf(buf + off, sz - off, ","); off += snprintf(buf + off, sz - off, "%s", bk[j]->s_name); }
            off += snprintf(buf + off, sz - off, "]"); if (bk) sysmem_freeptr(bk);
        }
        if (tk) sysmem_freeptr(tk); off += snprintf(buf + off, sz - off, "}");
    }
    off += snprintf(buf + off, sz - off, "}"); visualize((t_object *)x, buf); sysmem_freeptr(buf); dictobj_release(id);
}

t_max_err crucible_attr_set_visualize(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) { x->visualize = atom_getlong(av); if (x->visualize) crucible_visualize_state(x, NULL, NULL, NULL, 0.0, 1); }
    return MAX_ERR_NONE;
}

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

// --- Prototypes ---
void *buildspans_new(t_symbol *s, long argc, t_atom *argv);
void buildspans_free(t_buildspans *x);
void buildspans_clear(t_buildspans *x);
void buildspans_do_clear(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_do_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_float(t_buildspans *x, double f);
void buildspans_offset(t_buildspans *x, double f);
void buildspans_do_offset(t_buildspans *x, double f, double loop_start);
void buildspans_track(t_buildspans *x, long n);
void buildspans_do_track(t_buildspans *x, long n);
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_do_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv, long inlet_num);
void buildspans_bang(t_buildspans *x);
void buildspans_do_bang(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_flush(t_buildspans *x, t_symbol *palette_sym);
void buildspans_flush_track(t_buildspans *x, long track_num);
void buildspans_run_cleanup(t_buildspans *x);
void buildspans_end_track_span(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym);
void buildspans_prune_span(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, long bar_to_keep);
void buildspans_visualize_memory(t_buildspans *x);
void buildspans_log(t_buildspans *x, const char *fmt, ...);
void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_symbol *bar_sym);
void buildspans_finalize_and_log_span(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_atomarray *span_array);
void buildspans_deferred_rating_check(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, long last_bar_timestamp);
void buildspans_process_and_add_note(t_buildspans *x, double calc_timestamp, double store_timestamp, double score, double offset, long bar_length);
void buildspans_check_discontiguity(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, double relative_comparison_val);
void buildspans_cleanup_track_offset_if_needed(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_offset_sym);
double find_next_offset(t_buildspans *x, t_symbol *palette_sym, long track_num_to_check, double offset_val_to_check);
int buildspans_validate_span_before_output(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_atomarray *span_to_output);
void buildspans_output_span_data(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_atomarray *span_atom_array);
long buildspans_get_bar_length(t_buildspans *x);
void buildspans_set_bar_buffer(t_buildspans *x, t_symbol *s);
void buildspans_local_bar_length(t_buildspans *x, double f);
void buildspans_do_local_bar_length(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_bind_resolve(t_buildspans *x);
void buildspans_bind_clock_cb(t_buildspans *x);
void buildspans_notify(t_buildspans *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void buildspans_offset_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv);
void buildspans_track_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv);
void buildspans_anything_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv);
void buildspans_defer_output(t_buildspans *x, t_symbol *s, short argc, t_atom *argv);
t_max_err buildspans_attr_set_log(t_buildspans *x, void *attr, long ac, t_atom *av);
t_max_err buildspans_attr_set_async(t_buildspans *x, void *attr, long ac, t_atom *av);
t_max_err buildspans_attr_set_visualize(t_buildspans *x, void *attr, long ac, t_atom *av);
t_max_err buildspans_attr_set_bind(t_buildspans *x, void *attr, long ac, t_atom *av);

t_class *buildspans_class;

// --- Dictionary Helpers ---
t_dictionary* get_sub_dict(t_dictionary *parent, t_symbol *key, int create) {
    t_dictionary *sub = NULL;
    if (dictionary_getdictionary(parent, key, (t_object **)&sub) != MAX_ERR_NONE || !sub) {
        if (create) {
            sub = dictionary_new();
            dictionary_appenddictionary(parent, key, (t_object *)sub);
            dictionary_getdictionary(parent, key, (t_object **)&sub);
        }
    }
    return sub;
}

t_dictionary* buildspans_get_bar_dict(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_symbol *bar_sym, int create) {
    t_dictionary *pal_dict = get_sub_dict(x->building, palette_sym, create);
    if (!pal_dict) return NULL;
    t_dictionary *tr_dict = get_sub_dict(pal_dict, track_sym, create);
    if (!tr_dict) return NULL;
    return get_sub_dict(tr_dict, bar_sym, create);
}

int parse_hierarchical_key(t_symbol *hierarchical_key, t_symbol **palette, t_symbol **track, t_symbol **bar, t_symbol **key) {
    char buf[512]; strncpy(buf, hierarchical_key->s_name, 512); buf[511] = '\0';
    char *f = strstr(buf, "::"); if (!f) return 0; *f = '\0'; *palette = gensym(buf);
    char *s = strstr(f + 2, "::"); if (!s) return 0; *s = '\0'; *track = gensym(f + 2);
    char *t = strstr(s + 2, "::"); if (!t) return 0; *t = '\0'; *bar = gensym(s + 2);
    *key = gensym(t + 2); return 1;
}

t_symbol* generate_hierarchical_key(t_symbol *palette, t_symbol *track, t_symbol *bar, t_symbol *key) {
    char k[512]; snprintf(k, 512, "%s::%s::%s::%s", palette->s_name, track->s_name, bar->s_name, key->s_name);
    return gensym(k);
}

char* atomarray_to_string(t_atomarray *arr) {
    if (!arr) return NULL;
    long count = atomarray_getsize(arr);
    if (count == 0) { char *e = (char *)sysmem_newptr(3); strcpy(e, "[]"); return e; }
    long sz = 256; char *buf = (char *)sysmem_newptr(sz); long off = 0;
    off += snprintf(buf + off, sz - off, "[");
    for (long i = 0; i < count; i++) {
        t_atom a; atomarray_getindex(arr, i, &a); char ts[64];
        if (atom_gettype(&a) == A_FLOAT) snprintf(ts, 64, "%.2f", atom_getfloat(&a));
        else if (atom_gettype(&a) == A_LONG) snprintf(ts, 64, "%ld", atom_getlong(&a));
        else strcpy(ts, "?");
        if (sz - off < (long)strlen(ts) + 4) { sz *= 2; buf = (char *)sysmem_resizeptr(buf, sz); if (!buf) return NULL; }
        off += snprintf(buf + off, sz - off, "%s", ts);
        if (i < count - 1) off += snprintf(buf + off, sz - off, ", ");
    }
    snprintf(buf + off, sz - off, "]"); return buf;
}

t_atomarray* atomarray_deep_copy(t_atomarray *src) {
    if (!src) return NULL;
    long c; t_atom *a; atomarray_getatoms(src, &c, &a);
    return atomarray_new(c, a);
}

// Comparison functions
int compare_longs(const void *a, const void *b) { long la = *(const long *)a, lb = *(const long *)b; return (la > lb) - (la < lb); }
int compare_doubles(const void *a, const void *b) { double da = *(const double *)a, db = *(const double *)b; return (da > db) - (da < db); }
typedef struct { double timestamp; double score; } NotePair;
int compare_notepairs(const void *a, const void *b) { NotePair *pa = (NotePair *)a, *pb = (NotePair *)b; return (pa->timestamp > pb->timestamp) - (pa->timestamp < pb->timestamp); }
typedef struct { long track_number; double timestamp; double score; } t_duplication_manifest_item;
int compare_manifest_items(const void *a, const void *b) { t_duplication_manifest_item *pa = (t_duplication_manifest_item *)a, *pb = (t_duplication_manifest_item *)b; return (pa->timestamp > pb->timestamp) - (pa->timestamp < pb->timestamp); }

// --- Implementation ---
void buildspans_log(t_buildspans *x, const char *fmt, ...) { va_list args; va_start(args, fmt); vcommon_log(x->log_outlet, x->log, "buildspans", fmt, args); va_end(args); }

long buildspans_get_bar_length(t_buildspans *x) {
    if (x->local_bar_length > 0) return (long)x->local_bar_length;
    if (!x->buffer_ref) return -1;
    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        if (!x->bar_warn_sent) { object_warn((t_object *)x, "bar buffer~ not found"); x->bar_warn_sent = 1; }
        buffer_ref_set(x->buffer_ref, (x->s_buffer_name && x->s_buffer_name != _sym_nothing) ? x->s_buffer_name : gensym("bar"));
        b = buffer_ref_getobject(x->buffer_ref);
    }
    if (!b) return -1; x->bar_warn_sent = 0;
    long bl = 0; critical_enter(0); float *s = buffer_locksamples(b);
    if (s) { if (buffer_getframecount(b) > 0) bl = (long)s[0]; buffer_unlocksamples(b); }
    critical_exit(0); if (bl > 0) x->local_bar_length = (double)bl; return bl;
}

void buildspans_visualize_memory(t_buildspans *x) {
    if (!x->visualize) return;
    long sz = 65536; char *buf = (char *)sysmem_newptr(sz); long off = 0;
    off += snprintf(buf + off, sz - off, "{\"palettes\":{");
    long np = 0; t_symbol **pk = NULL; dictionary_getkeys(x->building, &np, &pk);
    for (long p = 0; p < np; p++) {
        t_dictionary *pd = NULL; dictionary_getdictionary(x->building, pk[p], (t_object **)&pd); if (!pd) continue;
        if (p > 0) off += snprintf(buf + off, sz - off, ",");
        off += snprintf(buf + off, sz - off, "\"%s\":{\"building\":{", pk[p]->s_name);
        long nt = 0; t_symbol **tk = NULL; dictionary_getkeys(pd, &nt, &tk);
        for (long i = 0; i < nt; i++) {
            t_dictionary *td = NULL; dictionary_getdictionary(pd, tk[i], (t_object **)&td); if (!td) continue;
            if (i > 0) off += snprintf(buf + off, sz - off, ",");
            off += snprintf(buf + off, sz - off, "\"%s\":{\"absolutes\":[", tk[i]->s_name);
            long nb = 0; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk);
            long *ts = (long *)sysmem_newptr(nb * sizeof(long)); for (long j = 0; j < nb; j++) ts[j] = atol(bk[j]->s_name);
            qsort(ts, nb, sizeof(long), compare_longs);
            int first_abs = 1;
            for (long j = 0; j < nb; j++) {
                char bs[32]; snprintf(bs, 32, "%ld", ts[j]); t_dictionary *bd = get_sub_dict(td, gensym(bs), 0);
                if (bd) {
                    t_atomarray *aa = NULL; if (dictionary_getatomarray(bd, gensym("absolutes"), (t_object **)&aa) == MAX_ERR_NONE && aa) {
                        long ac; t_atom *av; atomarray_getatoms(aa, &ac, &av);
                        for (long k = 0; k < ac; k++) { if (!first_abs) off += snprintf(buf + off, sz - off, ","); first_abs = 0; off += snprintf(buf + off, sz - off, "%.2f", atom_getfloat(av+k)); }
                    }
                }
            }
            off += snprintf(buf + off, sz - off, "],\"offsets\":[");
            int first_off = 1;
            for (long j = 0; j < nb; j++) {
                char bs[32]; snprintf(bs, 32, "%ld", ts[j]); t_dictionary *bd = get_sub_dict(td, gensym(bs), 0);
                if (bd) { double ov; if (dictionary_getfloat(bd, gensym("offset"), &ov) == MAX_ERR_NONE) { if (!first_off) off += snprintf(buf + off, sz - off, ","); first_off = 0; off += snprintf(buf + off, sz - off, "%.2f", ov); } }
            }
            off += snprintf(buf + off, sz - off, "],\"span\":[");
            if (nb > 0) {
                char bs[32]; snprintf(bs, 32, "%ld", ts[0]); t_dictionary *bd = get_sub_dict(td, gensym(bs), 0);
                if (bd) { t_atomarray *aa = NULL; if (dictionary_getatomarray(bd, gensym("span"), (t_object **)&aa) == MAX_ERR_NONE && aa) {
                    long ac; t_atom *av; atomarray_getatoms(aa, &ac, &av);
                    for (long k = 0; k < ac; k++) { if (k > 0) off += snprintf(buf + off, sz - off, ","); off += snprintf(buf + off, sz - off, "%ld", atom_getlong(av + k)); }
                } }
            }
            off += snprintf(buf + off, sz - off, "]}"); sysmem_freeptr(ts); if (bk) sysmem_freeptr(bk);
        }
        off += snprintf(buf + off, sz - off, "}}"); if (tk) sysmem_freeptr(tk);
    }
    if (pk) sysmem_freeptr(pk);
    off += snprintf(buf + off, sz - off, "},\"current_offset\":%.2f,\"bar_length\":%ld,\"loop_start\":%.2f}", x->current_offset, buildspans_get_bar_length(x), x->loop_start);
    visualize((t_object *)x, buf); sysmem_freeptr(buf);
}

void buildspans_defer_output(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) {
    if (s == gensym("track")) outlet_anything(x->track_outlet, s, argc, argv);
    else if (s == gensym("span")) outlet_anything(x->span_outlet, s, argc, argv);
    else if (s == gensym("bar_data")) { t_symbol *sel = atom_getsym(argv); outlet_anything(x->out_bar_data, sel, (short)(argc - 1), argv + 1); }
}

void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_symbol *bar_sym) {
    t_dictionary *bd = buildspans_get_bar_dict(x, palette_sym, track_sym, bar_sym, 0); if (!bd) return;
    double mv = 0; dictionary_getfloat(bd, gensym("mean"), &mv); dictionary_appendfloat(bd, gensym("rating"), mv);
    t_atomarray *ns = atomarray_new(0, NULL); t_atom a; atom_setlong(&a, atol(bar_sym->s_name)); atomarray_appendatom(ns, &a);
    dictionary_appendatomarray(bd, gensym("span"), (t_object *)ns);
}

void buildspans_finalize_and_log_span(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_atomarray *span_array) {
    if (!span_array) return; long sl = 0; t_atom *sa = NULL; atomarray_getatoms(span_array, &sl, &sa); if (sl == 0) return;
    t_dictionary *pd = get_sub_dict(x->building, palette_sym, 0); if (!pd) return;
    t_dictionary *td = get_sub_dict(pd, track_sym, 0); if (!td) return;
    double lm = -1.0;
    for (long i = 0; i < sl; i++) {
        char bs[32]; snprintf(bs, 32, "%ld", atom_getlong(sa + i)); t_dictionary *bd = get_sub_dict(td, gensym(bs), 0);
        if (bd) { double m = 0; if (dictionary_getfloat(bd, gensym("mean"), &m) == MAX_ERR_NONE) if (lm == -1.0 || m < lm) lm = m; }
    }
    double r = (lm != -1.0) ? (lm * sl) : 0.0;
    for (long i = 0; i < sl; i++) {
        char bs[32]; snprintf(bs, 32, "%ld", atom_getlong(sa + i)); t_dictionary *bd = get_sub_dict(td, gensym(bs), 0);
        if (bd) { dictionary_appendfloat(bd, gensym("rating"), r); dictionary_appendatomarray(bd, gensym("span"), (t_object *)atomarray_deep_copy(span_array)); }
    }
}

void buildspans_deferred_rating_check(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, long last_bar_timestamp) {
    t_dictionary *pd = get_sub_dict(x->building, palette_sym, 0); if (!pd) return;
    t_dictionary *td = get_sub_dict(pd, track_sym, 0); if (!td) return;
    long nb = 0; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk); if (!bk) return;
    if (nb > 1) {
        double lmw = -1.0, lmwo = -1.0, lbm = 0.0; long bwc = 0;
        for (long i = 0; i < nb; i++) {
            t_dictionary *bd = get_sub_dict(td, bk[i], 0); if (!bd) continue;
            double m = 0; dictionary_getfloat(bd, gensym("mean"), &m); long ts = atol(bk[i]->s_name);
            if (lmw == -1.0 || m < lmw) lmw = m;
            if (ts == last_bar_timestamp) lbm = m;
            else { if (lmwo == -1.0 || m < lmwo) lmwo = m; bwc++; }
        }
        if ((lmw * nb) < (lmwo * bwc) || lbm > (lmw * nb)) buildspans_prune_span(x, palette_sym, track_sym, last_bar_timestamp);
    }
    sysmem_freeptr(bk);
}

void buildspans_check_discontiguity(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, double relative_comparison_val) {
    long bl = buildspans_get_bar_length(x); if (bl <= 0) return;
    t_dictionary *pd = get_sub_dict(x->building, palette_sym, 0); if (!pd) return;
    t_dictionary *td = get_sub_dict(pd, track_sym, 0); if (!td) return;
    long last = -1, nb = 0; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk);
    if (bk) { for (long i = 0; i < nb; i++) { long v = atol(bk[i]->s_name); if (v > last) last = v; } sysmem_freeptr(bk); }
    if (last != -1) {
        buildspans_deferred_rating_check(x, palette_sym, track_sym, last);
        long rb = -1; dictionary_getkeys(td, &nb, &bk);
        if (bk) { for (long i = 0; i < nb; i++) { long v = atol(bk[i]->s_name); if (v > rb) rb = v; } sysmem_freeptr(bk); }
        if (rb != -1 && relative_comparison_val > (double)rb + 2.0 * bl) buildspans_end_track_span(x, palette_sym, track_sym);
    }
}

void buildspans_cleanup_track_offset_if_needed(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_offset_sym) {
    long tn; if (sscanf(track_offset_sym->s_name, "%ld-", &tn) != 1) return;
    t_dictionary *pd = get_sub_dict(x->building, palette_sym, 0); if (!pd) return;
    t_dictionary *td = get_sub_dict(pd, track_offset_sym, 0); if (!td) return;
    double ov = 0; long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk);
    if (bk) { if (nb > 0) { t_dictionary *bd = get_sub_dict(td, bk[0], 0); if (bd) dictionary_getfloat(bd, gensym("offset"), &ov); } }
    double no = find_next_offset(x, palette_sym, tn, ov);
    if (no == -1.0) { if (bk) sysmem_freeptr(bk); return; }
    double old = -1.0;
    for (long i = 0; i < nb; i++) {
        t_dictionary *bd = get_sub_dict(td, bk[i], 0); if (bd) {
            t_atomarray *aa = NULL; if (dictionary_getatomarray(bd, gensym("absolutes"), (t_object **)&aa) == MAX_ERR_NONE && aa) {
                long ac; t_atom *av; atomarray_getatoms(aa, &ac, &av);
                for (long k = 0; k < ac; k++) { double t = atom_getfloat(av + k); if (old == -1.0 || t < old) old = t; }
            }
        }
    }
    if (old != -1.0 && old >= no) { dictionary_deleteentry(pd, track_offset_sym); buildspans_visualize_memory(x); }
    if (bk) sysmem_freeptr(bk);
}

double find_next_offset(t_buildspans *x, t_symbol *palette_sym, long tn_check, double ov_check) {
    t_dictionary *pd = get_sub_dict(x->building, palette_sym, 0); if (!pd) return -1.0;
    long nt = 0; t_symbol **tk = NULL; dictionary_getkeys(pd, &nt, &tk); if (!tk) return -1.0;
    char p[32]; snprintf(p, 32, "%ld-", tn_check); t_dictionary *ud = dictionary_new();
    for (long i = 0; i < nt; i++) {
        if (strncmp(tk[i]->s_name, p, strlen(p)) == 0) {
            t_dictionary *td = NULL; dictionary_getdictionary(pd, tk[i], (t_object **)&td);
            if (td) {
                long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk);
                if (bk) {
                    if (nb > 0) { t_dictionary *bd = get_sub_dict(td, bk[0], 0); double ov; if (bd && dictionary_getfloat(bd, gensym("offset"), &ov) == MAX_ERR_NONE) { char ks[64]; snprintf(ks, 64, "%.2f", ov); dictionary_appendfloat(ud, gensym(ks), ov); } }
                    sysmem_freeptr(bk);
                }
            }
        }
    }
    sysmem_freeptr(tk);
    long nu = 0; t_symbol **ok = NULL; dictionary_getkeys(ud, &nu, &ok); double no = -1.0;
    if (ok) {
        double *os = (double *)sysmem_newptr(nu * sizeof(double));
        for (long i = 0; i < nu; i++) { t_atom a; dictionary_getatom(ud, ok[i], &a); os[i] = atom_getfloat(&a); }
        qsort(os, nu, sizeof(double), compare_doubles);
        for (long i = 0; i < nu; i++) if (os[i] > ov_check) { no = os[i]; break; }
        sysmem_freeptr(os); sysmem_freeptr(ok);
    }
    object_free(ud); return no;
}

int buildspans_validate_span_before_output(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_atomarray *span_to_output) {
    long tn; double ov = 0.0; if (sscanf(track_sym->s_name, "%ld-", &tn) != 1) return 0;
    long acs; t_atom *avs; atomarray_getatoms(span_to_output, &acs, &avs);
    if (acs > 0) { char bs[32]; snprintf(bs, 32, "%ld", atom_getlong(avs)); t_dictionary *bd = buildspans_get_bar_dict(x, palette_sym, track_sym, gensym(bs), 0); if (bd) dictionary_getfloat(bd, gensym("offset"), &ov); }
    double no = find_next_offset(x, palette_sym, tn, ov), ear = -1.0, lat = -1.0;
    for (long i = 0; i < acs; i++) {
        char bs[32]; snprintf(bs, 32, "%ld", atom_getlong(avs + i)); t_dictionary *bd = buildspans_get_bar_dict(x, palette_sym, track_sym, gensym(bs), 0);
        if (bd) {
            t_atomarray *aa = NULL; if (dictionary_getatomarray(bd, gensym("absolutes"), (t_object **)&aa) == MAX_ERR_NONE && aa) {
                long ac; t_atom *av; atomarray_getatoms(aa, &ac, &av);
                for (long k = 0; k < ac; k++) { double t = atom_getfloat(av + k); if (ear == -1.0 || t < ear) ear = t; if (lat == -1.0 || t > lat) lat = t; }
            }
        }
    }
    if (ear == -1.0 || lat < ov || (no != -1.0 && ear > no)) return 0; return 1;
}

void buildspans_output_span_data(t_buildspans *x, t_symbol *palette_sym, t_symbol *track_sym, t_atomarray *span_array) {
    if (!span_array) return; long tn; if (sscanf(track_sym->s_name, "%ld-", &tn) != 1) return;
    long sl; t_atom *sa; atomarray_getatoms(span_array, &sl, &sa);
    const char* pr[] = {"absolutes", "scores", "mean", "offset", "palette", "rating", "span"};
    for (long i = 0; i < sl; i++) {
        char bs[32]; snprintf(bs, 32, "%ld", atom_getlong(sa + i)); t_dictionary *bd = buildspans_get_bar_dict(x, palette_sym, track_sym, gensym(bs), 0); if (!bd) continue;
        for (int j = 0; j < 7; j++) {
            t_symbol *ps = gensym(pr[j]); t_atomarray *aa = NULL; t_atom a; t_atom *av = NULL; long ac = 0;
            if (dictionary_getatomarray(bd, ps, (t_object **)&aa) == MAX_ERR_NONE && aa) atomarray_getatoms(aa, &ac, &av);
            else if (dictionary_getatom(bd, ps, &a) == MAX_ERR_NONE) { av = &a; ac = 1; }
            if (ac > 0) {
                char ok[256]; snprintf(ok, 256, "%ld::%s::%s", tn, bs, pr[j]); t_symbol *os = gensym(ok);
                if (x->bound_crucible) crucible_do_anything((t_crucible *)x->bound_crucible, os, (short)ac, av);
                else {
                    if (!x->async || systhread_ismainthread()) outlet_anything(x->out_bar_data, os, (short)ac, av);
                    else { t_atom *nav = (t_atom *)sysmem_newptr((ac + 1) * sizeof(t_atom)); if (nav) { atom_setsym(nav, os); for (int k = 0; k < ac; k++) nav[k+1] = av[k]; defer(x, (method)buildspans_defer_output, gensym("bar_data"), (short)(ac + 1), nav); sysmem_freeptr(nav); } }
                }
            }
        }
    }
}

void buildspans_process_and_add_note(t_buildspans *x, double calc, double store, double score, double off, long bl) {
    if (off == 0.0) {
        t_dictionary *pd = get_sub_dict(x->building, x->current_palette, 0);
        if (pd) { char ts[64]; snprintf(ts, 64, "%ld-0", x->current_track); dictionary_deleteentry(pd, gensym(ts)); }
        buildspans_visualize_memory(x); return;
    }
    char ts[64]; snprintf(ts, 64, "%ld-%ld", x->current_track, (long)round(off)); t_symbol *tsym = gensym(ts);
    double rel = calc - off + x->loop_start; long bts = floor(rel / bl) * bl;
    char bs[32]; snprintf(bs, 32, "%ld", bts); t_symbol *bsym = gensym(bs);
    t_dictionary *pd = get_sub_dict(x->building, x->current_palette, 1), *td = get_sub_dict(pd, tsym, 1);
    long last = -1, nbc = 0; t_symbol **bks = NULL; dictionary_getkeys(td, &nbc, &bks);
    if (bks) { for (long i = 0; i < nbc; i++) { long v = atol(bks[i]->s_name); if (v > last) last = v; } sysmem_freeptr(bks); }
    if (bts > last && last != -1) buildspans_check_discontiguity(x, x->current_palette, tsym, rel);
    t_dictionary *bd = get_sub_dict(td, bsym, 1); dictionary_appendfloat(bd, gensym("offset"), off); dictionary_appendsym(bd, gensym("palette"), x->current_palette);
    t_atomarray *ab = NULL; if (dictionary_getatomarray(bd, gensym("absolutes"), (t_object **)&ab) != MAX_ERR_NONE || !ab) { ab = atomarray_new(0, NULL); dictionary_appendatomarray(bd, gensym("absolutes"), (t_object *)ab); }
    t_atom a; atom_setfloat(&a, store); atomarray_appendatom(ab, &a);
    t_atomarray *sc = NULL; if (dictionary_getatomarray(bd, gensym("scores"), (t_object **)&sc) != MAX_ERR_NONE || !sc) { sc = atomarray_new(0, NULL); dictionary_appendatomarray(bd, gensym("scores"), (t_object *)sc); }
    atom_setfloat(&a, score); atomarray_appendatom(sc, &a);
    long c = atomarray_getsize(sc); if (c > 0) { double sum = 0.0; t_atom *at; long ac; atomarray_getatoms(sc, &ac, &at); for (long i = 0; i < ac; i++) sum += atom_getfloat(at + i); dictionary_appendfloat(bd, gensym("mean"), sum / c); }
    dictionary_getkeys(td, &nbc, &bks);
    if (bks) {
        long *tss = (long *)sysmem_newptr(nbc * sizeof(long)); for (long i = 0; i < nbc; i++) tss[i] = atol(bks[i]->s_name);
        qsort(tss, nbc, sizeof(long), compare_longs);
        t_atomarray *ns = atomarray_new(0, NULL); for (long i = 0; i < nbc; i++) { t_atom ta; atom_setlong(&ta, tss[i]); atomarray_appendatom(ns, &ta); }
        double lm = -1.0;
        for (long i = 0; i < nbc; i++) { char bstr[32]; snprintf(bstr, 32, "%ld", tss[i]); t_dictionary *cbd = get_sub_dict(td, gensym(bstr), 0); if (cbd) { double m = 0; dictionary_getfloat(cbd, gensym("mean"), &m); if (lm == -1.0 || m < lm) lm = m; } }
        for (long i = 0; i < nbc; i++) { char bstr[32]; snprintf(bstr, 32, "%ld", tss[i]); t_dictionary *cbd = get_sub_dict(td, gensym(bstr), 0); if (cbd) { dictionary_appendatomarray(cbd, gensym("span"), (t_object *)atomarray_deep_copy(ns)); dictionary_appendfloat(cbd, gensym("rating"), lm * nbc); } }
        object_free(ns); sysmem_freeptr(tss); sysmem_freeptr(bks);
    }
    buildspans_visualize_memory(x);
}

void buildspans_end_track_span(t_buildspans *x, t_symbol *pal_sym, t_symbol *tr_sym) {
    t_dictionary *pd = get_sub_dict(x->building, pal_sym, 0); if (!pd) return;
    t_dictionary *td = get_sub_dict(pd, tr_sym, 0); if (!td) return;
    long nb = 0; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk); if (!bk) return;
    long *ts = (long *)sysmem_newptr(nb * sizeof(long)); for (long i = 0; i < nb; i++) ts[i] = atol(bk[i]->s_name);
    qsort(ts, nb, sizeof(long), compare_longs);
    t_atomarray *so = atomarray_new(0, NULL); for (long i = 0; i < nb; i++) { t_atom a; atom_setlong(&a, ts[i]); atomarray_appendatom(so, &a); }
    if (buildspans_validate_span_before_output(x, pal_sym, tr_sym, so)) {
        long tno; sscanf(tr_sym->s_name, "%ld-", &tno); buildspans_output_span_data(x, pal_sym, tr_sym, so);
        t_atom ta; atom_setlong(&ta, tno); if (x->bound_crucible) crucible_do_anything((t_crucible *)x->bound_crucible, gensym("track"), 1, &ta);
        else { if (!x->async || systhread_ismainthread()) outlet_anything(x->track_outlet, gensym("track"), 1, &ta); else defer(x, (method)buildspans_defer_output, gensym("track"), 1, &ta); }
        long ac; t_atom *av; atomarray_getatoms(so, &ac, &av); if (x->bound_crucible) crucible_do_anything((t_crucible *)x->bound_crucible, gensym("span"), (short)ac, av);
        else { if (!x->async || systhread_ismainthread()) outlet_anything(x->span_outlet, gensym("span"), (short)ac, av); else defer(x, (method)buildspans_defer_output, gensym("span"), (short)ac, av); }
    }
    object_free(so); sysmem_freeptr(ts); sysmem_freeptr(bk);
    dictionary_deleteentry(pd, tr_sym); buildspans_visualize_memory(x);
    char dk[256]; snprintf(dk, 256, "%s::%s", pal_sym->s_name, tr_sym->s_name); dictionary_appendsym(x->tracks_ended_in_current_event, gensym(dk), 0);
}

void buildspans_prune_span(t_buildspans *x, t_symbol *pal_sym, t_symbol *tr_sym, long btk) {
    t_dictionary *pd = get_sub_dict(x->building, pal_sym, 0); if (!pd) return;
    t_dictionary *td = get_sub_dict(pd, tr_sym, 0); if (!td) return;
    long nb = 0; t_symbol **bk = NULL; dictionary_getkeys(td, &nb, &bk); if (!bk) return;
    long ec = 0; long *bev = (long *)sysmem_newptr(nb * sizeof(long));
    for (long i = 0; i < nb; i++) { long val = atol(bk[i]->s_name); if (val != btk) bev[ec++] = val; }
    if (ec > 0) {
        qsort(bev, ec, sizeof(long), compare_longs);
        t_atomarray *esa = atomarray_new(0, NULL); for(long i = 0; i < ec; ++i) { t_atom a; atom_setlong(&a, bev[i]); atomarray_appendatom(esa, &a); }
        buildspans_finalize_and_log_span(x, pal_sym, tr_sym, esa);
        if (buildspans_validate_span_before_output(x, pal_sym, tr_sym, esa)) {
            long tno; sscanf(tr_sym->s_name, "%ld-", &tno); buildspans_output_span_data(x, pal_sym, tr_sym, esa);
            t_atom ta; atom_setlong(&ta, tno); if (x->bound_crucible) crucible_do_anything((t_crucible *)x->bound_crucible, gensym("track"), 1, &ta);
            else { if (!x->async || systhread_ismainthread()) outlet_anything(x->track_outlet, gensym("track"), 1, &ta); else defer(x, (method)buildspans_defer_output, gensym("track"), 1, &ta); }
            long ac; t_atom *av; atomarray_getatoms(esa, &ac, &av); if (x->bound_crucible) crucible_do_anything((t_crucible *)x->bound_crucible, gensym("span"), (short)ac, av);
            else { if (!x->async || systhread_ismainthread()) outlet_anything(x->span_outlet, gensym("span"), (short)ac, av); else defer(x, (method)buildspans_defer_output, gensym("span"), (short)ac, av); }
        }
        object_free(esa);
        for (long i = 0; i < ec; i++) { char bs[32]; snprintf(bs, 32, "%ld", bev[i]); dictionary_deleteentry(td, gensym(bs)); }
    }
    sysmem_freeptr(bev); sysmem_freeptr(bk);
    char bks[32]; snprintf(bks, 32, "%ld", btk); buildspans_reset_bar_to_standalone(x, pal_sym, tr_sym, gensym(bks));
    if (ec > 0) { char dk[256]; snprintf(dk, 256, "%s::%s", pal_sym->s_name, tr_sym->s_name); dictionary_appendsym(x->tracks_ended_in_current_event, gensym(dk), 0); }
    buildspans_visualize_memory(x);
}

void buildspans_do_offset(t_buildspans *x, double f, double ls) {
    if (x->async && x->worker && !systhread_ismainthread()) { t_atom a[2]; atom_setfloat(&a[0], f); atom_setfloat(&a[1], ls); async_worker_enqueue(x->worker, x, (method)buildspans_offset_deferred, NULL, 2, a); return; }
    if (x->defer && !systhread_ismainthread()) { t_atom a[2]; atom_setfloat(&a[0], f); atom_setfloat(&a[1], ls); defer(x, (method)buildspans_offset_deferred, NULL, 2, a); return; }
    if (f <= 0.0) { x->current_offset = f; x->loop_start = ls; x->last_msg_type = gensym("offset"); return; }
    long nr = (long)round(f), or = (long)round(x->current_offset); if (nr == or) { x->current_offset = f; x->loop_start = ls; return; }
    x->current_offset = f; x->loop_start = ls; x->last_msg_type = gensym("offset");
    long bl = buildspans_get_bar_length(x); if (bl <= 0) return;
    t_dictionary *pd = get_sub_dict(x->building, x->current_palette, 0); if (!pd) return;
    long nt = 0; t_symbol **tk = NULL; dictionary_getkeys(pd, &nt, &tk); if (!tk) return;
    long mc = 128, mcount = 0; t_duplication_manifest_item *m = (t_duplication_manifest_item *)sysmem_newptr(mc * sizeof(t_duplication_manifest_item));
    for (long i = 0; i < nt; i++) {
        t_symbol *ts = tk[i]; long tn; if (sscanf(ts->s_name, "%ld-", &tn) != 1) continue;
        buildspans_check_discontiguity(x, x->current_palette, ts, f);
        char tar[64]; snprintf(tar, 64, "%ld-%ld", tn, nr); if (dictionary_hasentry(pd, gensym(tar))) continue;
        char src[64]; snprintf(src, 64, "%ld-%ld", tn, or);
        if (strcmp(ts->s_name, src) == 0) {
            t_dictionary *td = NULL; dictionary_getdictionary(pd, ts, (t_object **)&td);
            if (td) {
                long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk);
                if (bk) {
                    for (long j = 0; j < nb; j++) {
                        t_dictionary *bd = get_sub_dict(td, bk[j], 0); if (bd) {
                            t_atomarray *ab = NULL, *sc = NULL; dictionary_getatomarray(bd, gensym("absolutes"), (t_object **)&ab); dictionary_getatomarray(bd, gensym("scores"), (t_object **)&sc);
                            if (ab && sc) {
                                long ac; t_atom *va, *vs; atomarray_getatoms(ab, &ac, &va); atomarray_getatoms(sc, &ac, &vs);
                                for (long k = 0; k < ac; k++) {
                                    if (mcount >= mc) { mc *= 2; m = (t_duplication_manifest_item *)sysmem_resizeptr(m, mc * sizeof(t_duplication_manifest_item)); }
                                    m[mcount].track_number = tn; m[mcount].timestamp = atom_getfloat(va + k); m[mcount].score = atom_getfloat(vs + k); mcount++;
                                }
                            }
                        }
                    }
                    sysmem_freeptr(bk);
                }
            }
        }
    }
    sysmem_freeptr(tk); buildspans_run_cleanup(x);
    if (mcount > 0) {
        qsort(m, mcount, sizeof(t_duplication_manifest_item), compare_manifest_items); long ot = x->current_track;
        for (long k = 0; k < mcount; k++) { x->current_track = m[k].track_number; buildspans_process_and_add_note(x, m[k].timestamp, m[k].timestamp, m[k].score, f, bl); }
        x->current_track = ot;
    }
    sysmem_freeptr(m); buildspans_visualize_memory(x);
}

void buildspans_do_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
    long bl = buildspans_get_bar_length(x); if (bl <= 0) return;
    double calc, store, score; if (argc == 2) { calc = atom_getfloat(argv); store = calc; score = atom_getfloat(argv+1); } else if (argc == 3) { calc = atom_getfloat(argv); score = atom_getfloat(argv+1); store = atom_getfloat(argv+2); } else return;
    double eff = (x->current_offset <= 0.0) ? calc : x->current_offset;
    t_dictionary *pd = get_sub_dict(x->building, x->current_palette, 1);
    long nt = 0; t_symbol **tk = NULL; dictionary_getkeys(pd, &nt, &tk);
    char p[32]; snprintf(p, 32, "%ld-", x->current_track);
    t_symbol **ts = (t_symbol **)sysmem_newptr((nt + 1) * sizeof(t_symbol *)); long tc = 0;
    if (tk) { for (long i = 0; i < nt; i++) if (strncmp(tk[i]->s_name, p, strlen(p)) == 0) ts[tc++] = tk[i]; sysmem_freeptr(tk); }
    char gs[64]; snprintf(gs, 64, "%ld-%ld", x->current_track, (long)round(eff)); t_symbol *g = gensym(gs); int f = 0;
    for (long i = 0; i < tc; i++) if (ts[i] == g) { f = 1; break; } if (!f) ts[tc++] = g;
    for (long i = 0; i < tc; i++) {
        double ao = eff; t_dictionary *td = get_sub_dict(pd, ts[i], 0);
        if (td) { long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk); if (bk) { if (nb > 0) { t_dictionary *bd = get_sub_dict(td, bk[0], 0); if (bd) dictionary_getfloat(bd, gensym("offset"), &ao); } sysmem_freeptr(bk); } }
        buildspans_process_and_add_note(x, calc, store, score, ao, bl);
    }
    sysmem_freeptr(ts); x->last_note_calc = calc; x->last_note_store = store; x->last_note_score = score; x->last_msg_type = gensym("list"); buildspans_run_cleanup(x);
}

void buildspans_run_cleanup(t_buildspans *x) {
    long ne; t_symbol **ek; dictionary_getkeys(x->tracks_ended_in_current_event, &ne, &ek);
    if (ek) { for (long i = 0; i < ne; i++) { char *p = NULL, *t = strstr(ek[i]->s_name, "::"); if (t) { size_t pl = t - ek[i]->s_name; p = (char *)sysmem_newptr(pl + 1); strncpy(p, ek[i]->s_name, pl); p[pl] = 0; buildspans_cleanup_track_offset_if_needed(x, gensym(p), gensym(t + 2)); sysmem_freeptr(p); } } sysmem_freeptr(ek); }
    dictionary_clear(x->tracks_ended_in_current_event);
}

void buildspans_bang(t_buildspans *x) { if (x->async && x->worker && !systhread_ismainthread()) { async_worker_enqueue(x->worker, x, (method)buildspans_do_bang, NULL, 0, NULL); return; } if (x->defer && !systhread_ismainthread()) { defer(x, (method)buildspans_do_bang, NULL, 0, NULL); return; } buildspans_do_bang(x, NULL, 0, NULL); }
void buildspans_do_bang(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { long bl = buildspans_get_bar_length(x); long nk; t_symbol **ks; dictionary_getkeys(x->building, &nk, &ks); if (ks) { for (long i = 0; i < nk; i++) buildspans_flush(x, ks[i]); sysmem_freeptr(ks); } x->local_bar_length = 0; x->last_msg_type = gensym("bang"); }
void buildspans_clear(t_buildspans *x) { if (x->async && x->worker && !systhread_ismainthread()) { async_worker_enqueue(x->worker, x, (method)buildspans_do_clear, NULL, 0, NULL); return; } if (x->defer && !systhread_ismainthread()) { defer(x, (method)buildspans_do_clear, NULL, 0, NULL); return; } buildspans_do_clear(x, NULL, 0, NULL); }
void buildspans_do_clear(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { if (x->building) object_free(x->building); if (x->tracks_ended_in_current_event) object_free(x->tracks_ended_in_current_event); x->building = dictionary_new(); x->tracks_ended_in_current_event = dictionary_new(); x->current_track = 0; x->current_offset = 0.0; x->loop_start = 0.0; x->current_palette = gensym(""); x->local_bar_length = 0; x->last_msg_type = gensym("clear"); buildspans_visualize_memory(x); }
void buildspans_offset(t_buildspans *x, double f) { if (x->async && x->worker && !systhread_ismainthread()) { t_atom a; atom_setfloat(&a, f); async_worker_enqueue(x->worker, x, (method)buildspans_offset_deferred, NULL, 1, &a); return; } buildspans_do_offset(x, f, 0.0); }
void buildspans_offset_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) { if (argc >= 2) buildspans_do_offset(x, atom_getfloat(argv), atom_getfloat(argv + 1)); else if (argc > 0) buildspans_do_offset(x, atom_getfloat(argv), 0.0); }
void buildspans_track_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) { if (argc > 0) buildspans_do_track(x, atom_getlong(argv)); }
void buildspans_track(t_buildspans *x, long n) { if (x->async && x->worker && !systhread_ismainthread()) { t_atom a; atom_setlong(&a, n); async_worker_enqueue(x->worker, x, (method)buildspans_track_deferred, NULL, 1, &a); return; } buildspans_do_track(x, n); }
void buildspans_do_track(t_buildspans *x, long n) { x->current_track = n; x->last_msg_type = gensym("track"); }
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { long il = proxy_getinlet((t_object *)x); if (x->async && x->worker && !systhread_ismainthread()) { t_atom *nav = (t_atom *)sysmem_newptr((argc + 1) * sizeof(t_atom)); if (nav) { atom_setlong(nav, il); for (long i = 0; i < argc; i++) nav[i+1] = argv[i]; async_worker_enqueue(x->worker, x, (method)buildspans_anything_deferred, s, argc + 1, nav); sysmem_freeptr(nav); } return; } buildspans_do_anything(x, s, argc, argv, il); }
void buildspans_anything_deferred(t_buildspans *x, t_symbol *s, short argc, t_atom *argv) { if (argc > 0) buildspans_do_anything(x, s, argc - 1, argv + 1, atom_getlong(argv)); }
void buildspans_do_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv, long il) { if (il == 3 && argc == 0) { x->current_palette = s; x->last_msg_type = gensym("palette"); } else if (il == 0 && s == gensym("flush") && argc > 0) buildspans_flush_track(x, atom_getlong(argv)); }
void buildspans_float(t_buildspans *x, double f) { long il = proxy_getinlet((t_object *)x); if (il == 1) buildspans_do_offset(x, f, 0.0); else if (il == 2) buildspans_track(x, (long)f); else if (il == 4) buildspans_local_bar_length(x, f); }
void buildspans_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { long il = proxy_getinlet((t_object *)x); if (x->async && x->worker && !systhread_ismainthread()) { if (il == 1) async_worker_enqueue(x->worker, x, (method)buildspans_offset_deferred, s, argc, argv); else async_worker_enqueue(x->worker, x, (method)buildspans_do_list, s, argc, argv); return; } if (il == 1) { if (argc >= 2) buildspans_do_offset(x, atom_getfloat(argv), atom_getfloat(argv + 1)); else if (argc == 1) buildspans_do_offset(x, atom_getfloat(argv), 0.0); return; } buildspans_do_list(x, s, argc, argv); }

void buildspans_assist(t_buildspans *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1: (list) note data, (bang) flush, (flush) flush track, (clear) clear, (log/visualize/bind/async) attributes."); break;
            case 1: sprintf(s, "Inlet 2: (list/float) Offset Timestamp, [Offset, Loop Start]"); break;
            case 2: sprintf(s, "Inlet 3: (int) Track Number"); break;
            case 3: sprintf(s, "Inlet 4: (symbol) Palette"); break;
            case 4: sprintf(s, "Inlet 5: (float) Local Bar Length"); break;
        }
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Outlet 1: Span Data (span list). Bypassed if @bind is active."); break;
            case 1: sprintf(s, "Outlet 2: Track Number (track int). Bypassed if @bind is active."); break;
            case 2: sprintf(s, "Outlet 3: Bar Data for Ended Spans (anything). Bypassed if @bind is active."); break;
            case 3: sprintf(s, "Outlet 4: Logging & Visualization Outlet"); break;
        }
    }
}

void buildspans_set_bar_buffer(t_buildspans *x, t_symbol *s) { if (s && s->s_name) { x->s_buffer_name = s; if (x->buffer_ref) buffer_ref_set(x->buffer_ref, s); else x->buffer_ref = buffer_ref_new((t_object *)x, s); } }
void buildspans_local_bar_length(t_buildspans *x, double f) { if (x->async && x->worker && !systhread_ismainthread()) { t_atom a; atom_setfloat(&a, f); async_worker_enqueue(x->worker, x, (method)buildspans_do_local_bar_length, NULL, 1, &a); return; } t_atom a; atom_setfloat(&a, f); buildspans_do_local_bar_length(x, NULL, 1, &a); }
void buildspans_do_local_bar_length(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) { double f = atom_getfloat(argv); x->local_bar_length = (f > 0) ? f : 0; if (x->bound_crucible) crucible_do_local_bar_length((t_crucible *)x->bound_crucible, NULL, 1, argv); }
t_max_err buildspans_attr_set_async(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) { x->async = atom_getlong(av); if (x->async && !x->worker) x->worker = async_worker_create(); else if (!x->async && x->worker) { async_worker_release(x->worker); x->worker = NULL; } } return MAX_ERR_NONE; }
t_max_err buildspans_attr_set_log(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) x->log = atom_getlong(av); return MAX_ERR_NONE; }

void buildspans_bind_resolve(t_buildspans *x) {
    t_object *p = NULL, *b = NULL, *o = NULL; t_symbol *vn = NULL; int found = 0;
    if (x->bind_name == _sym_nothing || x->bind_name == gensym("")) { if (x->bound_crucible) object_detach_byptr(x, x->bound_crucible); x->bound_crucible = NULL; x->bind_attempt_count = 0; return; }
    object_obex_lookup(x, gensym("#P"), &p); if (!p) { if (x->bind_clock) clock_delay(x->bind_clock, 100); return; }
    x->bind_attempt_count++;
    for (b = jpatcher_get_firstobject(p); b; b = jbox_get_nextobject(b)) {
        o = jbox_get_object(b); if (o) {
            vn = (t_symbol *)object_attr_getsym(b, gensym("varname"));
            if (vn == x->bind_name) {
                if (object_classname_compare(o, gensym("crucible")) || object_classname_compare(o, gensym("rebar_crucible_internal"))) {
                    t_symbol *cls = object_classname(o); if (!class_findbyname(CLASS_BOX, cls) && !class_findbyname(CLASS_NOBOX, cls)) continue;
                    if (x->bound_crucible && x->bound_crucible != o) object_detach_byptr(x, x->bound_crucible);
                    x->bound_crucible = o; object_attach_byptr(x, x->bound_crucible);
                    if (x->async) { t_crucible *c = (t_crucible *)o; if (c->async && c->worker) { if (x->worker) async_worker_release(x->worker); x->worker = c->worker; async_worker_retain(x->worker); } }
                    found = 1; break;
                }
            }
        }
    }
    if (found) { x->bind_attempt_count = 0; return; }
    if (x->bound_crucible) object_detach_byptr(x, x->bound_crucible); x->bound_crucible = NULL;
    if (x->bind_clock) clock_delay(x->bind_clock, 1000);
}
void buildspans_bind_clock_cb(t_buildspans *x) { if (x->bind_name != _sym_nothing && x->bind_name != gensym("") && !x->bound_crucible) buildspans_bind_resolve(x); }
void buildspans_notify(t_buildspans *x, t_symbol *s, t_symbol *msg, void *sender, void *data) { if (msg == gensym("free") && sender == x->bound_crucible) { x->bound_crucible = NULL; } }
t_max_err buildspans_attr_set_bind(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) { x->bind_name = atom_getsym(av); x->bind_attempt_count = 0; buildspans_bind_resolve(x); } return MAX_ERR_NONE; }
t_max_err buildspans_attr_set_visualize(t_buildspans *x, void *attr, long ac, t_atom *av) { if (ac && av) x->visualize = atom_getlong(av); return MAX_ERR_NONE; }

void buildspans_flush_track(t_buildspans *x, long track_num) {
    long bl = buildspans_get_bar_length(x); if (bl <= 0) return;
    long np = 0; t_symbol **pk = NULL; dictionary_getkeys(x->building, &np, &pk);
    if (!pk) return; char pref[32]; snprintf(pref, 32, "%ld-", track_num);
    for (long p = 0; p < np; p++) {
        t_dictionary *pd = NULL; dictionary_getdictionary(x->building, pk[p], (t_object **)&pd); if (!pd) continue;
        long nt = 0; t_symbol **tk = NULL; dictionary_getkeys(pd, &nt, &tk); if (!tk) continue;
        for (long i = 0; i < nt; i++) {
            if (strncmp(tk[i]->s_name, pref, strlen(pref)) == 0) {
                t_dictionary *td = NULL; dictionary_getdictionary(pd, tk[i], (t_object **)&td);
                if (td) {
                    long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk);
                    if (bk) {
                        long last = -1; for (long j = 0; j < nb; j++) { long v = atol(bk[j]->s_name); if (v > last) last = v; }
                        if (last != -1) buildspans_deferred_rating_check(x, pk[p], tk[i], last);
                        buildspans_end_track_span(x, pk[p], tk[i]); sysmem_freeptr(bk);
                    }
                }
            }
        }
        sysmem_freeptr(tk);
    }
    sysmem_freeptr(pk); x->last_msg_type = gensym("flush"); buildspans_run_cleanup(x);
}

void buildspans_flush(t_buildspans *x, t_symbol *pal_sym) {
    long bl = buildspans_get_bar_length(x); if (bl <= 0) return;
    t_dictionary *pd = get_sub_dict(x->building, pal_sym, 0); if (!pd) return;
    long nt = 0; t_symbol **tk = NULL; dictionary_getkeys(pd, &nt, &tk); if (!tk) return;
    for (long i = 0; i < nt; i++) {
        t_dictionary *td = NULL; dictionary_getdictionary(pd, tk[i], (t_object **)&td);
        if (td) {
            long nb; t_symbol **bk; dictionary_getkeys(td, &nb, &bk);
            if (bk) {
                long last = -1; for (long j = 0; j < nb; j++) { long v = atol(bk[j]->s_name); if (v > last) last = v; }
                if (last != -1) buildspans_deferred_rating_check(x, pal_sym, tk[i], last);
                buildspans_end_track_span(x, pal_sym, tk[i]); sysmem_freeptr(bk);
            }
        }
    }
    sysmem_freeptr(tk); x->last_msg_type = gensym("flush"); buildspans_run_cleanup(x);
}

void ext_main(void *r) {
    t_class *c = class_new("buildspans", (method)buildspans_new, (method)buildspans_free, sizeof(t_buildspans), 0L, A_GIMME, 0);
    class_addmethod(c, (method)buildspans_clear, "clear", 0);
    class_addmethod(c, (method)buildspans_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_track, "in2", A_LONG, 0);
    class_addmethod(c, (method)buildspans_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)buildspans_bang, "bang", 0);
    class_addmethod(c, (method)buildspans_set_bar_buffer, "set_bar_buffer", A_SYM, 0);
    class_addmethod(c, (method)buildspans_local_bar_length, "ft4", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_notify, "notify", A_CANT, 0);
    CLASS_ATTR_SYM(c, "bind", 0, t_buildspans, bind_name);
    CLASS_ATTR_ACCESSORS(c, "bind", NULL, (method)buildspans_attr_set_bind);
    CLASS_ATTR_LONG(c, "log", 0, t_buildspans, log); CLASS_ATTR_ACCESSORS(c, "log", NULL, (method)buildspans_attr_set_log);
    CLASS_ATTR_LONG(c, "visualize", 0, t_buildspans, visualize); CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)buildspans_attr_set_visualize);
    CLASS_ATTR_LONG(c, "async", 0, t_buildspans, async); CLASS_ATTR_ACCESSORS(c, "async", NULL, (method)buildspans_attr_set_async);
    class_register(CLASS_BOX, c); buildspans_class = c;
}

void *buildspans_new(t_symbol *s, long argc, t_atom *argv) {
    t_buildspans *x = (t_buildspans *)object_alloc(buildspans_class);
    if (x) {
        x->building = dictionary_new(); x->tracks_ended_in_current_event = dictionary_new();
        x->current_track = 0; x->current_offset = 0.0; x->loop_start = 0.0; x->current_palette = gensym("");
        x->log = 0; x->visualize = 0; x->async = 0; x->worker = NULL; x->buffer_ref = NULL; x->local_bar_length = 0;
        x->bind_name = _sym_nothing; x->bound_crucible = NULL; x->bind_clock = clock_new(x, (method)buildspans_bind_clock_cb); x->bind_attempt_count = 0;
        attr_args_process(x, argc, argv); buildspans_set_bar_buffer(x, gensym("bar"));
        floatin((t_object *)x, 4); proxy_new((t_object *)x, 3, NULL); intin((t_object *)x, 2); proxy_new((t_object *)x, 1, NULL);
        x->log_outlet = outlet_new((t_object *)x, NULL); visualize_init();
        x->out_bar_data = outlet_new((t_object *)x, NULL); x->track_outlet = outlet_new((t_object *)x, NULL); x->span_outlet = outlet_new((t_object *)x, NULL);
    }
    return (x);
}

void buildspans_free(t_buildspans *x) {
    visualize_cleanup(); if (x->worker) async_worker_release(x->worker);
    if (x->bind_clock) object_free(x->bind_clock); if (x->bound_crucible) object_detach_byptr(x, x->bound_crucible);
    if (x->building) object_free(x->building); if (x->tracks_ended_in_current_event) object_free(x->tracks_ended_in_current_event);
    if (x->buffer_ref) object_free(x->buffer_ref);
}

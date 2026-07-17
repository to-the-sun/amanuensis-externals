#include "crucible.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include "../shared/visualize.h"
#include "../shared/async_worker.h"
#include <string.h>
#include <ctype.h>
#include <math.h>

// Function prototypes
void crucible_do_rebar(t_crucible *x, t_atom_long new_bar_length);
void *crucible_new(t_symbol *s, long argc, t_atom *argv);
void crucible_free(t_crucible *x);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_log(t_crucible *x, const char *fmt, ...);
char *crucible_atoms_to_string(long argc, t_atom *argv);
int parse_selector(const char *selector_str, char **track, char **bar, char **key);
t_dictionary *dictionary_deep_copy(t_dictionary *src);
void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, t_atom_long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict);
void crucible_local_bar_length(t_crucible *x, double f);
void crucible_do_local_bar_length(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
t_max_err crucible_attr_set_log(t_crucible *x, void *attr, long ac, t_atom *av);
t_max_err crucible_attr_set_async(t_crucible *x, void *attr, long ac, t_atom *av);
t_max_err crucible_attr_set_consume(t_crucible *x, void *attr, long ac, t_atom *av);
t_max_err crucible_attr_set_fill(t_crucible *x, void *attr, long ac, t_atom *av);
t_atom_long crucible_get_bar_length(t_crucible *x);
t_atomarray *crucible_get_span_as_atomarray(t_dictionary *bar_dict);
int crucible_span_has_loser(t_atomarray *span_aa, t_dictionary *defeated_dict);
void crucible_recalculate_reaches(t_crucible *x);
void crucible_visualize_dump_all_spans(t_crucible *x);
void crucible_visualize_state(t_crucible *x, t_symbol *event_type, t_symbol *track_id_sym, t_atomarray *span_aa, double rating, int include_tracks);
t_max_err crucible_attr_set_visualize(t_crucible *x, void *attr, long ac, t_atom *av);

// Dyn String helper struct and prototypes
typedef struct {
    char *data;
    long size;
    long capacity;
} t_dyn_str;

void dyn_str_init(t_dyn_str *ds, long initial_cap);
void dyn_str_free(t_dyn_str *ds);
void dyn_str_append(t_dyn_str *ds, const char *str);
void dyn_str_append_char(t_dyn_str *ds, char c);
void dyn_str_append_printf(t_dyn_str *ds, const char *fmt, ...);
void serialize_atom(t_dyn_str *ds, t_atom *a);
void serialize_atomarray(t_dyn_str *ds, t_atomarray *aa);
void serialize_dict(t_dyn_str *ds, t_dictionary *dict);
void crucible_visualize_repopulate(t_crucible *x);


#ifndef REBAR_INTERNAL_BINDING
t_class *crucible_class;
#endif

void get_track_bounds(t_dictionary *track_dict, t_atom_long bar_length, t_atom_long *out_min, t_atom_long *out_max, int *out_has_bars) {
    if (!track_dict) {
        *out_has_bars = 0;
        return;
    }
    t_symbol **bar_keys = NULL;
    long num_bars = 0;
    dictionary_getkeys(track_dict, &num_bars, &bar_keys);
    if (num_bars == 0) {
        if (bar_keys) sysmem_freeptr(bar_keys);
        *out_has_bars = 0;
        return;
    }
    t_atom_long min_ts = 0;
    t_atom_long max_ts = 0;
    int first = 1;
    for (long j = 0; j < num_bars; j++) {
        t_symbol *bar_sym = bar_keys[j];
        const char *name = bar_sym->s_name;
        if (!name || name[0] == '\0') continue;
        int is_num = 1;
        int k = 0;
        if (name[0] == '-') k = 1;
        for (; name[k]; k++) {
            if (!isdigit(name[k])) {
                is_num = 0;
                break;
            }
        }
        if (!is_num) continue;

        t_atom_long bar_ts = atoll(name);
        if (first) {
            min_ts = bar_ts;
            max_ts = bar_ts;
            first = 0;
        } else {
            if (bar_ts < min_ts) min_ts = bar_ts;
            if (bar_ts > max_ts) max_ts = bar_ts;
        }
    }
    if (bar_keys) sysmem_freeptr(bar_keys);
    if (first) {
        *out_has_bars = 0;
    } else {
        *out_min = min_ts;
        *out_max = max_ts;
        *out_has_bars = 1;
    }
}

int crucible_compare_longs(const void *a, const void *b) {
    t_atom_long la = *(const t_atom_long *)a;
    t_atom_long lb = *(const t_atom_long *)b;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

t_atom_long *get_sorted_track_bars(t_dictionary *track_dict, long *out_count) {
    t_symbol **bar_keys = NULL;
    long num_bars = 0;
    dictionary_getkeys(track_dict, &num_bars, &bar_keys);
    if (num_bars == 0) {
        if (bar_keys) sysmem_freeptr(bar_keys);
        *out_count = 0;
        return NULL;
    }
    t_atom_long *bars = (t_atom_long *)sysmem_newptr(num_bars * sizeof(t_atom_long));
    long count = 0;
    for (long j = 0; j < num_bars; j++) {
        t_symbol *bar_sym = bar_keys[j];
        const char *name = bar_sym->s_name;
        if (!name || name[0] == '\0') continue;
        int is_num = 1;
        int k = 0;
        if (name[0] == '-') k = 1;
        for (; name[k]; k++) {
            if (!isdigit(name[k])) {
                is_num = 0;
                break;
            }
        }
        if (!is_num) continue;
        bars[count++] = atoll(name);
    }
    if (bar_keys) sysmem_freeptr(bar_keys);
    if (count == 0) {
        sysmem_freeptr(bars);
        *out_count = 0;
        return NULL;
    }
    qsort(bars, count, sizeof(t_atom_long), crucible_compare_longs);
    *out_count = count;
    return bars;
}

void adjust_filled_bar_dict(t_dictionary *bar_dict, t_atom_long src_ts, t_atom_long dest_ts) {
    // No-op: do not shift any internal values of the copied bar
}

void crucible_defer_output(t_crucible *x, t_symbol *s, short argc, t_atom *argv) {
    if (s == gensym("-")) {
        outlet_anything(x->outlet_data, s, argc, argv);
    } else if (s == gensym("data_list")) {
        outlet_list(x->outlet_data, NULL, argc, argv);
    } else if (s == gensym("reach_song")) {
        outlet_anything(x->outlet_reach_int, gensym("song"), argc, argv);
    } else if (s == gensym("reach_list")) {
        outlet_list(x->outlet_reach_int, NULL, argc, argv);
    } else if (s == gensym("fill")) {
        outlet_anything(x->outlet_fill, gensym("fill"), 0, NULL);
    }
}

void crucible_log(t_crucible *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "crucible", fmt, args);
    va_end(args);
}

char *crucible_atoms_to_string(long argc, t_atom *argv) {
    if (argc == 0 || !argv) {
        char *empty_str = (char *)sysmem_newptr(3);
        strcpy(empty_str, "[]");
        return empty_str;
    }

    long buffer_size = 256;
    char *buffer = (char *)sysmem_newptr(buffer_size);
    long offset = 0;

    offset += snprintf(buffer + offset, buffer_size - offset, "[");

    for (long i = 0; i < argc; i++) {
        char temp_str[128];
        if (atom_gettype(argv + i) == A_FLOAT) {
            snprintf(temp_str, 128, "%.2f", atom_getfloat(argv + i));
        } else if (atom_gettype(argv + i) == A_LONG) {
            snprintf(temp_str, 128, "%lld", (long long)atom_getlong(argv + i));
        } else if (atom_gettype(argv + i) == A_SYM) {
            snprintf(temp_str, 128, "%s", atom_getsym(argv + i)->s_name);
        } else {
            snprintf(temp_str, 128, "?");
        }

        if (buffer_size - offset < (long)strlen(temp_str) + 4) {
            buffer_size *= 2;
            buffer = (char *)sysmem_resizeptr(buffer, buffer_size);
            if (!buffer) return NULL;
        }

        offset += snprintf(buffer + offset, buffer_size - offset, "%s", temp_str);
        if (i < argc - 1) {
            offset += snprintf(buffer + offset, buffer_size - offset, ", ");
        }
    }

    snprintf(buffer + offset, buffer_size - offset, "]");
    return buffer;
}

int parse_selector(const char *selector_str, char **track, char **bar, char **key) {
    const char *first_delim = strstr(selector_str, "::");
    if (!first_delim) return 0;
    const char *second_delim = strstr(first_delim + 2, "::");
    if (!second_delim) return 0;

    size_t track_len = first_delim - selector_str;
    *track = (char *)sysmem_newptr(track_len + 1);
    if (!*track) return 0;
    strncpy(*track, selector_str, track_len);
    (*track)[track_len] = '\0';

    size_t bar_len = second_delim - (first_delim + 2);
    *bar = (char *)sysmem_newptr(bar_len + 1);
    if (!*bar) {
        sysmem_freeptr(*track);
        return 0;
    }
    strncpy(*bar, first_delim + 2, bar_len);
    (*bar)[bar_len] = '\0';

    *key = (char *)sysmem_newptr(strlen(second_delim + 2) + 1);
    if (!*key) {
        sysmem_freeptr(*track);
        sysmem_freeptr(*bar);
        return 0;
    }
    strcpy(*key, second_delim + 2);

    return 1;
}

static void crucible_main_hidden(void *r) {
    common_symbols_init();
    t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, sizeof(t_crucible), 0L, A_GIMME, 0);
    class_addmethod(c, (method)crucible_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)crucible_local_bar_length, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)crucible_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_crucible, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "log", NULL, (method)crucible_attr_set_log);

    CLASS_ATTR_LONG(c, "consume", 0, t_crucible, consume);
    CLASS_ATTR_STYLE_LABEL(c, "consume", 0, "onoff", "Enable Consume");
    CLASS_ATTR_DEFAULT(c, "consume", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "consume", NULL, (method)crucible_attr_set_consume);

    CLASS_ATTR_LONG(c, "fill", 0, t_crucible, fill);
    CLASS_ATTR_STYLE_LABEL(c, "fill", 0, "onoff", "Enable Fill");
    CLASS_ATTR_DEFAULT(c, "fill", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "fill", NULL, (method)crucible_attr_set_fill);

    CLASS_ATTR_LONG(c, "defer", 0, t_crucible, defer);
    CLASS_ATTR_STYLE_LABEL(c, "defer", 0, "onoff", "Deferred Execution");
    CLASS_ATTR_DEFAULT(c, "defer", 0, "0");

    CLASS_ATTR_LONG(c, "async", 0, t_crucible, async);
    CLASS_ATTR_STYLE_LABEL(c, "async", 0, "onoff", "Asynchronous Execution");
    CLASS_ATTR_DEFAULT(c, "async", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "async", NULL, (method)crucible_attr_set_async);

    CLASS_ATTR_LONG(c, "visualize", 0, t_crucible, visualize);
    CLASS_ATTR_STYLE_LABEL(c, "visualize", 0, "onoff", "Enable Visualization");
    CLASS_ATTR_DEFAULT(c, "visualize", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)crucible_attr_set_visualize);

    class_register(CLASS_BOX, c);
    crucible_class = c;
}

#ifndef NO_EXT_MAIN
void ext_main(void *r) {
    crucible_main_hidden(r);
}
#endif

void *crucible_new(t_symbol *s, long argc, t_atom *argv) {
    t_crucible *x = (t_crucible *)object_alloc(crucible_class);
    if (x) {
        visualize_init();
        x->challenger_dict = dictionary_new();
        x->last_track_id = gensym("");
        x->incumbent_dict_name = gensym("");
        x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        if (!buffer_ref_getobject(x->buffer_ref)) {
            object_error((t_object *)x, "bar buffer~ not found");
        }
        x->log = 0;
        x->consume = 0;
        x->defer = 0;
        x->async = 0;
        x->worker = NULL;
        x->visualize = 0;
        x->fill = 0;
        x->song_reach = 0;
        x->track_reaches_dict = dictionary_new();
        x->local_bar_length = 0;
        x->instance_id = 1000 + (rand() % 9000);
        x->bar_warn_sent = 0;

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->incumbent_dict_name = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        // Outlets are created from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->outlet_reach_int = outlet_new((t_object *)x, NULL);   // Index 2
        x->outlet_fill = outlet_new((t_object *)x, NULL);        // Index 1
        x->outlet_data = outlet_new((t_object *)x, NULL);        // Index 0

        floatin((t_object *)x, 1);
    }
    return (x);
}

void crucible_free(t_crucible *x) {
    visualize_cleanup();
    if (x->worker) {
        async_worker_release(x->worker);
    }
    if (x->challenger_dict) {
        object_release((t_object *)x->challenger_dict);
    }
    if (x->track_reaches_dict) {
        object_release((t_object *)x->track_reaches_dict);
    }
    if (x->buffer_ref) {
        object_free(x->buffer_ref);
    }
}

void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, t_atom_long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict) {
    t_atom_long bar_length = crucible_get_bar_length(x);
    crucible_log(x, "crucible_output_bar_data: utilizing bar_length %lld", (long long)bar_length);
    if (!bar_dict) return;

    t_atom *span_atoms = NULL;
    long span_len = 0;
    t_atomarray *span_aa = NULL;
    t_atom span_atom;

    if (dictionary_getatomarray(bar_dict, gensym("span"), (t_object **)&span_aa) == MAX_ERR_NONE && span_aa) {
        atomarray_getatoms(span_aa, &span_len, &span_atoms);
    } else if (dictionary_getatom(bar_dict, gensym("span"), &span_atom) == MAX_ERR_NONE) {
        span_atoms = &span_atom;
        span_len = 1;
    } else {
        object_error((t_object *)x, "Missing 'span' key for bar %lld on track %s", (long long)bar_ts_long, track_sym->s_name);
    }

    // Right-to-Left execution order: Reach, Offset, Bar, Track, Palette

    // 1. Reach
    if (span_len > 0) {
        t_atom_long max_val = 0;
        for (long j = 0; j < span_len; j++) {
            t_atom_long current_val = atom_getlong(span_atoms + j);
            if (current_val > max_val) max_val = current_val;
        }

        t_atom_long bar_length = crucible_get_bar_length(x);
        t_atom_long track_min = 0, track_max = 0;
        int track_has = 0;
        get_track_bounds(incumbent_track_dict, bar_length, &track_min, &track_max, &track_has);
        t_atom_long current_reach = (max_val + bar_length) - (track_has ? track_min : 0);

        char reach_str[64];
        snprintf(reach_str, 64, "%lld", (long long)current_reach);

        crucible_log(x, "Checking reach %lld for track %s", (long long)current_reach, track_sym->s_name);
        if (incumbent_track_dict && !dictionary_hasentry(incumbent_track_dict, gensym(reach_str))) {
            crucible_log(x, "  -> Reach %lld not found in incumbent. Sending reach message.", (long long)current_reach);
            t_atom reach_list[3];
            atom_setlong(reach_list, (t_atom_long)atol(track_sym->s_name));
            atom_setlong(reach_list + 1, current_reach);
            atom_setfloat(reach_list + 2, -999999.0);

            if (x->outlet_data) {
                if (!x->async || systhread_ismainthread()) {
                    outlet_anything(x->outlet_data, gensym("-"), 3, reach_list);
                } else {
                    defer(x, (method)crucible_defer_output, gensym("-"), 3, reach_list);
                }
            }
        }
    }

    // 2. Data List: [palette, track, bar, offset]
    t_atom list[4];
    t_symbol *palette_sym = _sym_nothing;
    double offset_val = 0.0;

    t_atom *palette_atoms = NULL;
    long palette_len = 0;
    t_atomarray *palette_aa = NULL;
    t_atom palette_atom;

    if (dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_aa) == MAX_ERR_NONE && palette_aa) {
        atomarray_getatoms(palette_aa, &palette_len, &palette_atoms);
    } else if (dictionary_getatom(bar_dict, gensym("palette"), &palette_atom) == MAX_ERR_NONE) {
        palette_atoms = &palette_atom;
        palette_len = 1;
    } else {
        object_error((t_object *)x, "Missing 'palette' key for bar %lld on track %s", (long long)bar_ts_long, track_sym->s_name);
    }
    if (palette_len > 0 && atom_gettype(palette_atoms) == A_SYM) palette_sym = atom_getsym(palette_atoms);

    t_atom *offset_atoms = NULL;
    long offset_len = 0;
    t_atomarray *offset_aa = NULL;
    t_atom offset_atom;

    if (dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_aa) == MAX_ERR_NONE && offset_aa) {
        atomarray_getatoms(offset_aa, &offset_len, &offset_atoms);
    } else if (dictionary_getatom(bar_dict, gensym("offset"), &offset_atom) == MAX_ERR_NONE) {
        offset_atoms = &offset_atom;
        offset_len = 1;
    } else {
        object_error((t_object *)x, "Missing 'offset' key for bar %lld on track %s", (long long)bar_ts_long, track_sym->s_name);
    }
    if (offset_len > 0) {
        if (atom_gettype(offset_atoms) == A_FLOAT) {
            offset_val = atom_getfloat(offset_atoms);
        } else if (atom_gettype(offset_atoms) == A_LONG) {
            offset_val = (double)atom_getlong(offset_atoms);
        }
    }

    atom_setsym(list, palette_sym);
    atom_setlong(list + 1, (t_atom_long)atol(track_sym->s_name));
    atom_setlong(list + 2, bar_ts_long);
    atom_setfloat(list + 3, offset_val);

    if (x->outlet_data) {
        if (!x->async || systhread_ismainthread()) {
            outlet_list(x->outlet_data, NULL, 4, list);
        } else {
            defer(x, (method)crucible_defer_output, gensym("data_list"), 4, list);
        }
    }
}

void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray) {
    t_atom_long bar_length = crucible_get_bar_length(x);
    crucible_log(x, "crucible_process_span: utilizing bar_length %lld", (long long)bar_length);
    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) {
        object_error((t_object *)x, "could not find dictionary named %s", x->incumbent_dict_name->s_name);
        return;
    }

    long span_len = 0;
    t_atom *span_atoms = NULL;
    atomarray_getatoms(span_atomarray, &span_len, &span_atoms);

    crucible_log(x, "Processing span for track %s with %ld bars", track_sym->s_name, span_len);

    int challenger_wins = 1;
    double challenger_winning_rating = 0.0;

    // Get challenger track dictionary
    t_dictionary *challenger_track_dict = NULL;
    dictionary_getdictionary(x->challenger_dict, track_sym, (t_object **)&challenger_track_dict);
    if (!challenger_track_dict) {
        object_error((t_object *)x, "Could not find challenger track dict for %s", track_sym->s_name);
        goto cleanup;
    }

    for (long i = 0; i < span_len; i++) {
        t_atom_long bar_ts_long = atom_getlong(&span_atoms[i]);
        char bar_ts_str[64];
        snprintf(bar_ts_str, 64, "%lld", (long long)bar_ts_long);
        t_symbol *bar_sym = gensym(bar_ts_str);

        // Get challenger bar dictionary
        t_dictionary *challenger_bar_dict = NULL;
        dictionary_getdictionary(challenger_track_dict, bar_sym, (t_object **)&challenger_bar_dict);
        if (!challenger_bar_dict) {
            object_error((t_object *)x, "Missing challenger bar dictionary for bar %s", bar_sym->s_name);
            continue;
        }

        t_atom *challenger_rating_atoms = NULL;
        long challenger_rating_len = 0;
        t_atomarray *challenger_rating_aa = NULL;
        t_atom challenger_rating_atom;

        if (dictionary_getatomarray(challenger_bar_dict, gensym("rating"), (t_object **)&challenger_rating_aa) == MAX_ERR_NONE && challenger_rating_aa) {
            atomarray_getatoms(challenger_rating_aa, &challenger_rating_len, &challenger_rating_atoms);
        } else if (dictionary_getatom(challenger_bar_dict, gensym("rating"), &challenger_rating_atom) == MAX_ERR_NONE) {
            challenger_rating_atoms = &challenger_rating_atom;
            challenger_rating_len = 1;
        }
        if (challenger_rating_len == 0) {
            object_error((t_object *)x, "Missing rating for challenger bar %s", bar_sym->s_name);
            continue;
        }
        double challenger_rating = atom_getfloat(challenger_rating_atoms);

        if (i == 0) {
            challenger_winning_rating = challenger_rating;
        }


        // Check against incumbent
        t_dictionary *incumbent_track_dict = NULL;
        if (dictionary_hasentry(incumbent_dict, track_sym)) {
            dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict);
        }

        if (incumbent_track_dict && dictionary_hasentry(incumbent_track_dict, bar_sym)) {
            t_dictionary *incumbent_bar_dict = NULL;
            dictionary_getdictionary(incumbent_track_dict, bar_sym, (t_object **)&incumbent_bar_dict);

            t_atom *incumbent_rating_atoms = NULL;
            long incumbent_rating_len = 0;
            t_atomarray *incumbent_rating_aa = NULL;
            t_atom incumbent_rating_atom;

            if (dictionary_getatomarray(incumbent_bar_dict, gensym("rating"), (t_object **)&incumbent_rating_aa) == MAX_ERR_NONE && incumbent_rating_aa) {
                atomarray_getatoms(incumbent_rating_aa, &incumbent_rating_len, &incumbent_rating_atoms);
            } else if (dictionary_getatom(incumbent_bar_dict, gensym("rating"), &incumbent_rating_atom) == MAX_ERR_NONE) {
                incumbent_rating_atoms = &incumbent_rating_atom;
                incumbent_rating_len = 1;
            }

            if (incumbent_rating_len == 0) {
                crucible_log(x, "Bar %lld: Challenger rating %.2f vs Incumbent (no-contest, missing or empty rating). Challenger wins bar.", (long long)bar_ts_long, challenger_rating);
                continue;
            }

            double incumbent_rating = atom_getfloat(incumbent_rating_atoms);
            crucible_log(x, "Bar %lld: Challenger rating %.2f vs Incumbent rating %.2f.", (long long)bar_ts_long, challenger_rating, incumbent_rating);
            if (challenger_rating <= incumbent_rating) {
                crucible_log(x, "-> Challenger loses bar. Span comparison failed.");
                challenger_wins = 0;
                break;
            } else {
                crucible_log(x, "-> Challenger wins bar.");
            }
        } else {
            crucible_log(x, "Bar %lld: Challenger rating %.2f vs Incumbent (no-contest, no entry). Challenger wins bar.", (long long)bar_ts_long, challenger_rating);
        }
    }

    t_dictionary *incumbent_track_dict = NULL;
    t_dictionary *defeated_dict = NULL;
    t_dictionary *challenger_span_ts_dict = NULL;
    t_dictionary *old_reaches = NULL;
    int track_grew = 0;
    int song_grew = 0;

    // Song-wide boundaries before the won challenger bars are written
    t_symbol **all_track_keys = NULL;
    long num_all_tracks = 0;
    t_atom_long song_prev_min = 0;
    t_atom_long song_prev_max = 0;
    int song_had_bars = 0;

    if (incumbent_dict) {
        dictionary_getkeys(incumbent_dict, &num_all_tracks, &all_track_keys);

        for (long t = 0; t < num_all_tracks; t++) {
            t_dictionary *tr_dict = NULL;
            dictionary_getdictionary(incumbent_dict, all_track_keys[t], (t_object **)&tr_dict);
            if (tr_dict) {
                t_atom_long t_min = 0, t_max = 0;
                int t_has = 0;
                get_track_bounds(tr_dict, bar_length, &t_min, &t_max, &t_has);
                if (t_has) {
                    if (!song_had_bars) {
                        song_prev_min = t_min;
                        song_prev_max = t_max;
                        song_had_bars = 1;
                    } else {
                        if (t_min < song_prev_min) song_prev_min = t_min;
                        if (t_max > song_prev_max) song_prev_max = t_max;
                    }
                }
            }
        }
    }

    if (challenger_wins) {
        crucible_log(x, "Challenger span for track %s won. Overwriting incumbent dictionary.", track_sym->s_name);

        defeated_dict = dictionary_new();
        challenger_span_ts_dict = dictionary_new();

        for (long i = 0; i < span_len; i++) {
            t_atom_long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[64];
            snprintf(bar_ts_str, 64, "%lld", (long long)bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            dictionary_appendlong(challenger_span_ts_dict, bar_sym, 1);

            // Check if this bar replaces an incumbent bar
            if (dictionary_hasentry(incumbent_dict, track_sym)) {
                t_dictionary *temp_incumbent_track_dict = NULL;
                dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&temp_incumbent_track_dict);
                if (temp_incumbent_track_dict && dictionary_hasentry(temp_incumbent_track_dict, bar_sym)) {
                    dictionary_appendlong(defeated_dict, bar_sym, 1);
                }
            }
        }

        // Get or create incumbent track dictionary
        if (!dictionary_hasentry(incumbent_dict, track_sym)) {
            incumbent_track_dict = dictionary_new();
            dictionary_appenddictionary(incumbent_dict, track_sym, (t_object *)incumbent_track_dict);
            // Re-retrieve to ensure we have the internal pointer
            dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict);
        } else {
            dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict);
        }

        // Deep Delete logic
        t_atom_long num_defeated = dictionary_getentrycount(defeated_dict);
        if (x->consume && num_defeated > 0) {
            crucible_log(x, "Performing deep delete (consume enabled). %lld bars directly defeated.", (long long)num_defeated);

            t_dictionary *to_delete_dict = dictionary_new();
            t_symbol **defeated_keys = NULL;
            long num_defeated_keys = 0;
            dictionary_getkeys(defeated_dict, &num_defeated_keys, &defeated_keys);

            for (long i = 0; i < num_defeated_keys; i++) {
                t_symbol *defeated_bar_sym = defeated_keys[i];
                t_dictionary *defeated_bar_dict = NULL;
                dictionary_getdictionary(incumbent_track_dict, defeated_bar_sym, (t_object **)&defeated_bar_dict);
                if (defeated_bar_dict) {
                    t_atomarray *item_span_aa = crucible_get_span_as_atomarray(defeated_bar_dict);
                    if (item_span_aa) {
                        long item_span_count = 0;
                        t_atom *item_span_atoms = NULL;
                        atomarray_getatoms(item_span_aa, &item_span_count, &item_span_atoms);
                        for (long j = 0; j < item_span_count; j++) {
                            t_atom_long ts = atom_getlong(item_span_atoms + j);
                            char ts_str[64];
                            snprintf(ts_str, 64, "%lld", (long long)ts);
                            t_symbol *ts_sym = gensym(ts_str);
                            if (!dictionary_hasentry(challenger_span_ts_dict, ts_sym)) {
                                dictionary_appendlong(to_delete_dict, ts_sym, 1);
                            }
                        }
                        object_release((t_object *)item_span_aa);
                    }
                }
            }

            // Now perform the delete
            t_symbol **delete_keys = NULL;
            long num_delete_keys = 0;
            dictionary_getkeys(to_delete_dict, &num_delete_keys, &delete_keys);
            for (long i = 0; i < num_delete_keys; i++) {
                t_symbol *del_bar_sym = delete_keys[i];
                t_dictionary *del_bar_dict = NULL;
                dictionary_getdictionary(incumbent_track_dict, del_bar_sym, (t_object **)&del_bar_dict);
                if (del_bar_dict) {
                    t_atomarray *del_span_aa = crucible_get_span_as_atomarray(del_bar_dict);
                    if (crucible_span_has_loser(del_span_aa, defeated_dict)) {
                        crucible_log(x, "  -> Consuming bar %s (part of a defeated span)", del_bar_sym->s_name);
                        dictionary_deleteentry(incumbent_track_dict, del_bar_sym);
                    }
                    if (del_span_aa) object_release((t_object *)del_span_aa);
                }
            }
            if (delete_keys) sysmem_freeptr(delete_keys);
            if (defeated_keys) sysmem_freeptr(defeated_keys);
            object_release((t_object *)to_delete_dict);
        }

        // Copy bars to incumbent
        for (long i = 0; i < span_len; i++) {
            t_atom_long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[64];
            snprintf(bar_ts_str, 64, "%lld", (long long)bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            t_dictionary *challenger_bar_dict = NULL;
            dictionary_getdictionary(challenger_track_dict, bar_sym, (t_object **)&challenger_bar_dict);

            if (challenger_bar_dict) {
                if (dictionary_hasentry(incumbent_track_dict, bar_sym)) {
                     dictionary_deleteentry(incumbent_track_dict, bar_sym);
                }
                dictionary_appenddictionary(incumbent_track_dict, bar_sym, (t_object *)dictionary_deep_copy(challenger_bar_dict));
                crucible_log(x, "  -> Wrote bar %s to incumbent track %s", bar_sym->s_name, track_sym->s_name);
            }
        }

        crucible_log(x, "[Fill Debug] x->fill = %ld, song_had_bars = %d, bar_length = %lld", x->fill, song_had_bars, (long long)bar_length);
        if (song_had_bars) {
            crucible_log(x, "[Fill Debug] song_prev_min = %lld, song_prev_max = %lld", (long long)song_prev_min, (long long)song_prev_max);
        }
        if (incumbent_track_dict) {
            t_atom_long win_min = 0, win_max = 0;
            int win_has = 0;
            get_track_bounds(incumbent_track_dict, bar_length, &win_min, &win_max, &win_has);
            crucible_log(x, "[Fill Debug] win_has = %d, win_min = %lld, win_max = %lld", win_has, (long long)win_min, (long long)win_max);
        }

        if (x->fill && incumbent_dict) {
            // Recalculate song boundaries after the winner is written
            t_atom_long song_curr_min = 0;
            t_atom_long song_curr_max = 0;
            int song_has = 0;

            for (long t = 0; t < num_all_tracks; t++) {
                t_dictionary *tr_dict = NULL;
                dictionary_getdictionary(incumbent_dict, all_track_keys[t], (t_object **)&tr_dict);
                if (tr_dict) {
                    t_atom_long t_min = 0, t_max = 0;
                    int t_has = 0;
                    get_track_bounds(tr_dict, bar_length, &t_min, &t_max, &t_has);
                    if (t_has) {
                        if (!song_has) {
                            song_curr_min = t_min;
                            song_curr_max = t_max;
                            song_has = 1;
                        } else {
                            if (t_min < song_curr_min) song_curr_min = t_min;
                            if (t_max > song_curr_max) song_curr_max = t_max;
                        }
                    }
                }
            }

            if (song_has) {
                crucible_log(x, "Filling tracks to match song bounds: [%lld, %lld]", (long long)song_curr_min, (long long)song_curr_max);
                for (long t = 0; t < num_all_tracks; t++) {
                    t_symbol *other_track_sym = all_track_keys[t];
                    t_dictionary *other_track_dict = NULL;
                    dictionary_getdictionary(incumbent_dict, other_track_sym, (t_object **)&other_track_dict);
                    if (other_track_dict) {
                        t_atom_long o_min = 0, o_max = 0;
                        int o_has = 0;
                        get_track_bounds(other_track_dict, bar_length, &o_min, &o_max, &o_has);
                        if (o_has) {
                            if (o_max < song_curr_max) {
                                long o_bars_count = 0;
                                t_atom_long *o_bars = get_sorted_track_bars(other_track_dict, &o_bars_count);
                                if (o_bars && o_bars_count > 0) {
                                    long k = 0;
                                    for (t_atom_long dest_ts = o_max + bar_length; dest_ts <= song_curr_max; dest_ts += bar_length) {
                                        t_atom_long src_ts = o_bars[k % o_bars_count];
                                        char src_ts_str[64];
                                        snprintf(src_ts_str, 64, "%lld", (long long)src_ts);
                                        t_dictionary *src_bar_dict = NULL;
                                        dictionary_getdictionary(other_track_dict, gensym(src_ts_str), (t_object **)&src_bar_dict);
                                        if (src_bar_dict) {
                                            t_dictionary *copied_bar_dict = dictionary_deep_copy(src_bar_dict);
                                            adjust_filled_bar_dict(copied_bar_dict, src_ts, dest_ts);

                                            char dest_ts_str[64];
                                            snprintf(dest_ts_str, 64, "%lld", (long long)dest_ts);
                                            t_symbol *dest_bar_sym = gensym(dest_ts_str);

                                            if (dictionary_hasentry(other_track_dict, dest_bar_sym)) {
                                                dictionary_deleteentry(other_track_dict, dest_bar_sym);
                                            }
                                            dictionary_appenddictionary(other_track_dict, dest_bar_sym, (t_object *)copied_bar_dict);
                                            crucible_log(x, "  [Fill Pos] Copied track %s bar %lld to %lld", other_track_sym->s_name, (long long)src_ts, (long long)dest_ts);

                                            crucible_output_bar_data(x, copied_bar_dict, dest_ts, other_track_sym, other_track_dict);

                                            if (x->visualize) {
                                                char vis_msg[256];
                                                snprintf(vis_msg, 256, "{\"event\":\"fill_bar\",\"track\":\"%s\",\"bar\":%lld,\"copied_from\":%lld}",
                                                         other_track_sym->s_name, (long long)dest_ts, (long long)src_ts);
                                                visualize((t_object *)x, vis_msg);
                                            }
                                            object_release((t_object *)copied_bar_dict);
                                        }
                                        k++;
                                    }
                                    sysmem_freeptr(o_bars);
                                }
                            }

                            if (o_min > song_curr_min) {
                                long o_bars_count = 0;
                                t_atom_long *o_bars = get_sorted_track_bars(other_track_dict, &o_bars_count);
                                if (o_bars && o_bars_count > 0) {
                                    long k = 0;
                                    for (t_atom_long dest_ts = o_min - bar_length; dest_ts >= song_curr_min; dest_ts -= bar_length) {
                                        long src_idx = o_bars_count - 1 - (k % o_bars_count);
                                        t_atom_long src_ts = o_bars[src_idx];
                                        char src_ts_str[64];
                                        snprintf(src_ts_str, 64, "%lld", (long long)src_ts);
                                        t_dictionary *src_bar_dict = NULL;
                                        dictionary_getdictionary(other_track_dict, gensym(src_ts_str), (t_object **)&src_bar_dict);
                                        if (src_bar_dict) {
                                            t_dictionary *copied_bar_dict = dictionary_deep_copy(src_bar_dict);
                                            adjust_filled_bar_dict(copied_bar_dict, src_ts, dest_ts);

                                            char dest_ts_str[64];
                                            snprintf(dest_ts_str, 64, "%lld", (long long)dest_ts);
                                            t_symbol *dest_bar_sym = gensym(dest_ts_str);

                                            if (dictionary_hasentry(other_track_dict, dest_bar_sym)) {
                                                dictionary_deleteentry(other_track_dict, dest_bar_sym);
                                            }
                                            dictionary_appenddictionary(other_track_dict, dest_bar_sym, (t_object *)copied_bar_dict);
                                            crucible_log(x, "  [Fill Neg] Copied track %s bar %lld to %lld", other_track_sym->s_name, (long long)src_ts, (long long)dest_ts);

                                            crucible_output_bar_data(x, copied_bar_dict, dest_ts, other_track_sym, other_track_dict);

                                            if (x->visualize) {
                                                char vis_msg[256];
                                                snprintf(vis_msg, 256, "{\"event\":\"fill_bar\",\"track\":\"%s\",\"bar\":%lld,\"copied_from\":%lld}",
                                                         other_track_sym->s_name, (long long)dest_ts, (long long)src_ts);
                                                visualize((t_object *)x, vis_msg);
                                            }
                                            object_release((t_object *)copied_bar_dict);
                                        }
                                        k++;
                                    }
                                    sysmem_freeptr(o_bars);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Recalculate reaches and save old ones
        t_atom_long old_song_reach = x->song_reach;
        old_reaches = dictionary_new();
        t_symbol **tr_keys = NULL;
        long num_tr = 0;
        dictionary_getkeys(x->track_reaches_dict, &num_tr, &tr_keys);
        for (long t = 0; t < num_tr; t++) {
            t_atom_long r = 0;
            dictionary_getlong(x->track_reaches_dict, tr_keys[t], &r);
            dictionary_appendlong(old_reaches, tr_keys[t], r);
        }
        if (tr_keys) sysmem_freeptr(tr_keys);

        crucible_recalculate_reaches(x);

        track_grew = 0;
        dictionary_getkeys(x->track_reaches_dict, &num_tr, &tr_keys);
        for (long t = 0; t < num_tr; t++) {
            t_symbol *tr_sym = tr_keys[t];
            t_atom_long new_r = 0;
            dictionary_getlong(x->track_reaches_dict, tr_sym, &new_r);
            t_atom_long old_r = 0;
            dictionary_getlong(old_reaches, tr_sym, &old_r);
            if (new_r > old_r) {
                track_grew = 1;
            }
        }
        if (tr_keys) sysmem_freeptr(tr_keys);

        song_grew = (x->song_reach > old_song_reach);

        if (x->visualize) {
            // Send entire repopulate dictionary first, then send the span packet to trigger animation/pop-up
            crucible_visualize_repopulate(x);
            crucible_visualize_state(x, gensym("new_span"), track_sym, span_atomarray, challenger_winning_rating, 0);
        }
    } else {
        crucible_log(x, "Challenger span for track %s lost.", track_sym->s_name);
    }

    // CLEAN SLATE: Cleanup challenger dict for this track IMMEDIATELY after update
    if (dictionary_hasentry(x->challenger_dict, track_sym)) {
        dictionary_deleteentry(x->challenger_dict, track_sym);
        crucible_log(x, "Cleaned up challenger data for track %s.", track_sym->s_name);
    }

    // Now handle output if it won
    if (challenger_wins && incumbent_track_dict) {
        // Standard Max right-to-left outlet firing order:
        // Reach Lists (Index 2) -> Fill (Index 1) -> Data (Index 0)
        if (song_grew || track_grew) {
            if (x->outlet_reach_int) {
                if (song_grew) {
                    t_atom reach_atom;
                    atom_setlong(&reach_atom, (t_atom_long)x->song_reach);
                    if (!x->async || systhread_ismainthread()) {
                        outlet_anything(x->outlet_reach_int, gensym("song"), 1, &reach_atom);
                    } else {
                        defer(x, (method)crucible_defer_output, gensym("reach_song"), 1, &reach_atom);
                    }
                }
                t_symbol **tr_keys = NULL;
                long num_tr = 0;
                dictionary_getkeys(x->track_reaches_dict, &num_tr, &tr_keys);
                for (long t = 0; t < num_tr; t++) {
                    t_symbol *tr_sym = tr_keys[t];
                    t_atom_long new_r = 0;
                    dictionary_getlong(x->track_reaches_dict, tr_sym, &new_r);
                    t_atom_long old_r = 0;
                    dictionary_getlong(old_reaches, tr_sym, &old_r);
                    if (new_r > old_r) {
                        t_atom reach_list[2];
                        atom_setlong(reach_list, (t_atom_long)atol(tr_sym->s_name));
                        atom_setlong(reach_list + 1, new_r);
                        if (!x->async || systhread_ismainthread()) {
                            outlet_list(x->outlet_reach_int, NULL, 2, reach_list);
                        } else {
                            defer(x, (method)crucible_defer_output, gensym("reach_list"), 2, reach_list);
                        }
                    }
                }
                if (tr_keys) sysmem_freeptr(tr_keys);
            }
            if (song_grew && x->outlet_fill) {
                if (!x->async || systhread_ismainthread()) {
                    outlet_anything(x->outlet_fill, gensym("fill"), 0, NULL);
                } else {
                    defer(x, (method)crucible_defer_output, gensym("fill"), 0, NULL);
                }
            }
        }
        if (old_reaches) object_free(old_reaches);

        for (long i = 0; i < span_len; i++) {
            t_atom_long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[64];
            snprintf(bar_ts_str, 64, "%lld", (long long)bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(incumbent_track_dict, bar_sym, (t_object **)&bar_dict);
            if (bar_dict) {
                crucible_output_bar_data(x, bar_dict, bar_ts_long, track_sym, incumbent_track_dict);
            }
        }
    }

cleanup:
    if (defeated_dict) object_release((t_object *)defeated_dict);
    if (challenger_span_ts_dict) object_release((t_object *)challenger_span_ts_dict);
    if (incumbent_dict) {
        object_release((t_object *)incumbent_dict);
    }
    if (all_track_keys) sysmem_freeptr(all_track_keys);
}

t_atom_long crucible_get_bar_length(t_crucible *x) {
    if (x->local_bar_length > 0) {
        return (t_atom_long)x->local_bar_length;
    }

    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        if (!x->bar_warn_sent) {
            object_warn((t_object *)x, "bar buffer~ not found, attempting to kick reference");
            x->bar_warn_sent = 1;
        }
        // Kick the buffer reference to force re-binding
        buffer_ref_set(x->buffer_ref, _sym_nothing);
        buffer_ref_set(x->buffer_ref, gensym("bar"));
        b = buffer_ref_getobject(x->buffer_ref);
    }
    if (!b) {
        object_error((t_object *)x, "bar buffer~ not found");
        return 0;
    }
    x->bar_warn_sent = 0; // Reset flag when buffer is successfully found

    t_atom_long bar_length = 0;
    critical_enter(0);
    float *samples = buffer_locksamples(b);
    if (samples) {
        if (buffer_getframecount(b) > 0) {
            bar_length = (t_atom_long)samples[0];
        }
        buffer_unlocksamples(b);
        critical_exit(0);
    } else {
        critical_exit(0);
    }

    if (bar_length > 0) {
        if (bar_length != (t_atom_long)x->local_bar_length) {
            crucible_log(x, "bar_length changed to %lld", (long long)bar_length);
        }
        x->local_bar_length = (double)bar_length;
    }

    return bar_length;
}

void crucible_local_bar_length(t_crucible *x, double f) {
    if (x->async && x->worker && !async_worker_is_worker_thread(x->worker)) {
        t_atom a;
        atom_setfloat(&a, f);
        async_worker_enqueue(x->worker, x, (method)crucible_do_local_bar_length, NULL, 1, &a);
        return;
    }
    if (x->defer && !systhread_ismainthread()) {
        t_atom a;
        atom_setfloat(&a, f);
        defer(x, (method)crucible_do_local_bar_length, NULL, 1, &a);
        return;
    }
    t_atom a;
    atom_setfloat(&a, f);
    crucible_do_local_bar_length(x, NULL, 1, &a);
}

void crucible_do_local_bar_length(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    double f = atom_getfloat(argv);
    long long old_bar_length = (long long)x->local_bar_length;
    if (f <= 0) {
        x->local_bar_length = 0;
    } else {
        x->local_bar_length = f;
    }
    if ((long long)x->local_bar_length != old_bar_length) {
        crucible_log(x, "bar_length changed to %lld", (long long)x->local_bar_length);
        if (x->visualize && x->local_bar_length > 0) {
            char msg[128];
            snprintf(msg, 128, "{\"event\":\"cleanup\",\"bar_length\":%lld}", (long long)x->local_bar_length);
            visualize((t_object *)x, msg);
        }
    }
}

t_max_err crucible_attr_set_async(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->async = atom_getlong(av);
        if (x->async && !x->worker) {
            x->worker = async_worker_create();
        } else if (!x->async && x->worker) {
            async_worker_release(x->worker);
            x->worker = NULL;
        }
    }
    return MAX_ERR_NONE;
}

// Dynamic string / Serialization Implementation
void dyn_str_init(t_dyn_str *ds, long initial_cap) {
    if (initial_cap <= 0) initial_cap = 256;
    ds->data = (char *)sysmem_newptr(initial_cap);
    ds->data[0] = '\0';
    ds->size = 0;
    ds->capacity = initial_cap;
}

void dyn_str_free(t_dyn_str *ds) {
    if (ds->data) {
        sysmem_freeptr(ds->data);
        ds->data = NULL;
    }
    ds->size = 0;
    ds->capacity = 0;
}

void dyn_str_append(t_dyn_str *ds, const char *str) {
    if (!str) return;
    long len = strlen(str);
    while (ds->size + len >= ds->capacity) {
        ds->capacity *= 2;
        ds->data = (char *)sysmem_resizeptr(ds->data, ds->capacity);
    }
    strcpy(ds->data + ds->size, str);
    ds->size += len;
}

void dyn_str_append_char(t_dyn_str *ds, char c) {
    while (ds->size + 1 >= ds->capacity) {
        ds->capacity *= 2;
        ds->data = (char *)sysmem_resizeptr(ds->data, ds->capacity);
    }
    ds->data[ds->size] = c;
    ds->size++;
    ds->data[ds->size] = '\0';
}

void dyn_str_append_printf(t_dyn_str *ds, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    dyn_str_append(ds, buf);
}

void serialize_atom(t_dyn_str *ds, t_atom *a) {
    if (!a) {
        dyn_str_append(ds, "null");
        return;
    }
    switch (atom_gettype(a)) {
        case A_LONG:
            dyn_str_append_printf(ds, "%lld", (long long)atom_getlong(a));
            break;
        case A_FLOAT:
            dyn_str_append_printf(ds, "%.6f", atom_getfloat(a));
            break;
        case A_SYM: {
            t_symbol *sym = atom_getsym(a);
            if (sym && sym->s_name) {
                // Escape string if needed, otherwise output inside quotes.
                // Simple quoting is usually safe for names, keys, palettes.
                dyn_str_append_char(ds, '"');
                // Escape simple quotes and backslashes if present
                for (const char *p = sym->s_name; *p; p++) {
                    if (*p == '"' || *p == '\\') {
                        dyn_str_append_char(ds, '\\');
                    }
                    dyn_str_append_char(ds, *p);
                }
                dyn_str_append_char(ds, '"');
            } else {
                dyn_str_append(ds, "\"\"");
            }
            break;
        }
        case A_OBJ: {
            t_object *obj = atom_getobj(a);
            if (obj) {
                if (object_classname_compare(obj, gensym("dictionary"))) {
                    serialize_dict(ds, (t_dictionary *)obj);
                } else if (object_classname_compare(obj, gensym("atomarray"))) {
                    serialize_atomarray(ds, (t_atomarray *)obj);
                } else {
                    dyn_str_append(ds, "{}");
                }
            } else {
                dyn_str_append(ds, "null");
            }
            break;
        }
        default:
            dyn_str_append(ds, "null");
            break;
    }
}

void serialize_atomarray(t_dyn_str *ds, t_atomarray *aa) {
    if (!aa) {
        dyn_str_append(ds, "[]");
        return;
    }
    long ac = 0;
    t_atom *av = NULL;
    atomarray_getatoms(aa, &ac, &av);
    dyn_str_append_char(ds, '[');
    for (long i = 0; i < ac; i++) {
        if (i > 0) dyn_str_append_char(ds, ',');
        serialize_atom(ds, av + i);
    }
    dyn_str_append_char(ds, ']');
}

void serialize_dict(t_dyn_str *ds, t_dictionary *dict) {
    if (!dict) {
        dyn_str_append(ds, "{}");
        return;
    }
    t_symbol **keys = NULL;
    long num_keys = 0;
    dictionary_getkeys(dict, &num_keys, &keys);
    dyn_str_append_char(ds, '{');
    for (long i = 0; i < num_keys; i++) {
        if (i > 0) dyn_str_append_char(ds, ',');
        t_symbol *k = keys[i];

        // Output key in quotes
        dyn_str_append_char(ds, '"');
        dyn_str_append(ds, k->s_name);
        dyn_str_append(ds, "\":");

        t_atom val;
        dictionary_getatom(dict, k, &val);
        serialize_atom(ds, &val);
    }
    dyn_str_append_char(ds, '}');
    if (keys) {
        sysmem_freeptr(keys);
    }
}

void crucible_visualize_repopulate(t_crucible *x) {
    if (!x->visualize) return;
    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) return;

    t_dyn_str ds;
    dyn_str_init(&ds, 32768);

    t_atom_long bar_length = crucible_get_bar_length(x);

    dyn_str_append_printf(&ds, "{\"event\":\"repopulate\",\"bar_length\":%lld,\"dictionary\":", (long long)bar_length);
    serialize_dict(&ds, incumbent_dict);
    dyn_str_append_char(&ds, '}');

    visualize((t_object *)x, ds.data);

    dyn_str_free(&ds);
    dictobj_release(incumbent_dict);
}

t_max_err crucible_attr_set_fill(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->fill = atom_getlong(av);
        crucible_log(x, "fill attribute set to %ld", x->fill);
    }
    return MAX_ERR_NONE;
}

t_max_err crucible_attr_set_log(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->log = atom_getlong(av);
        crucible_log(x, "log attribute set to %ld", x->log);
    }
    return MAX_ERR_NONE;
}

t_max_err crucible_attr_set_consume(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->consume = atom_getlong(av);
        crucible_log(x, "consume attribute set to %ld", x->consume);
    }
    return MAX_ERR_NONE;
}

t_dictionary *dictionary_deep_copy(t_dictionary *src) {
   if (!src) return NULL;

   t_dictionary *dest = dictionary_new();
   if (!dest) return NULL;
   t_symbol **keys = NULL;
   long numkeys = 0;

   dictionary_getkeys(src, &numkeys, &keys);

   for (long i = 0; i < numkeys; i++) {
       t_symbol *key = keys[i];
       t_atom value;

       dictionary_getatom(src, key, &value);

       if (atom_gettype(&value) == A_OBJ) {
           t_object *obj = atom_getobj(&value);
           if (obj) {
               if (object_classname_compare(obj, gensym("dictionary"))) {
                   t_dictionary *nested_src = (t_dictionary *)obj;
                   t_dictionary *nested_dest = dictionary_deep_copy(nested_src);
                   if (nested_dest) dictionary_appenddictionary(dest, key, (t_object *)nested_dest);
               } else if (object_classname_compare(obj, gensym("atomarray"))) {
                   t_atomarray *aa_src = (t_atomarray *)obj;
                   long aa_len = 0;
                   t_atom *aa_atoms = NULL;
                   atomarray_getatoms(aa_src, &aa_len, &aa_atoms);
                   t_atomarray *aa_dest = atomarray_new(aa_len, aa_atoms);
                   if (aa_dest) dictionary_appendatomarray(dest, key, (t_object *)aa_dest);
               }
           }
       } else {
           dictionary_appendatom(dest, key, &value);
       }
   }

   if (keys) {
       sysmem_freeptr(keys);
   }

   return dest;
}

void crucible_recalculate_reaches(t_crucible *x) {
    t_atom_long bar_length = crucible_get_bar_length(x);
    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) return;

    x->song_reach = 0;
    dictionary_clear(x->track_reaches_dict);

    t_symbol **track_keys = NULL;
    long num_tracks = 0;
    dictionary_getkeys(incumbent_dict, &num_tracks, &track_keys);

    t_atom_long song_min = 0;
    t_atom_long song_max = 0;
    int song_has = 0;

    for (long i = 0; i < num_tracks; i++) {
        t_symbol *track_sym = track_keys[i];
        t_dictionary *track_dict = NULL;
        dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&track_dict);
        if (!track_dict) continue;

        t_atom_long track_min = 0;
        t_atom_long track_max = 0;
        int track_has = 0;
        get_track_bounds(track_dict, bar_length, &track_min, &track_max, &track_has);

        if (track_has) {
            t_atom_long track_reach = (track_max + bar_length) - track_min;
            if (dictionary_hasentry(x->track_reaches_dict, track_sym)) {
                dictionary_deleteentry(x->track_reaches_dict, track_sym);
            }
            dictionary_appendlong(x->track_reaches_dict, track_sym, track_reach);

            if (!song_has) {
                song_min = track_min;
                song_max = track_max;
                song_has = 1;
            } else {
                if (track_min < song_min) song_min = track_min;
                if (track_max > song_max) song_max = track_max;
            }
        }
    }

    if (song_has) {
        x->song_reach = (song_max + bar_length) - song_min;
    }

    if (track_keys) sysmem_freeptr(track_keys);
    dictobj_release(incumbent_dict);
}

void crucible_visualize_dump_all_spans(t_crucible *x) {
    if (!x->visualize) return;
    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) return;

    t_symbol **track_keys = NULL;
    long num_tracks = 0;
    dictionary_getkeys(incumbent_dict, &num_tracks, &track_keys);

    for (long i = 0; i < num_tracks; i++) {
        t_symbol *t_sym = track_keys[i];
        t_dictionary *track_dict = NULL;
        if (dictionary_getdictionary(incumbent_dict, t_sym, (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) continue;

        t_symbol **bar_keys = NULL;
        long num_bars = 0;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        t_dictionary *seen_spans = dictionary_new();

        for (long j = 0; j < num_bars; j++) {
            t_symbol *bar_sym = bar_keys[j];
            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
            if (!bar_dict) continue;

            t_atomarray *span_aa = crucible_get_span_as_atomarray(bar_dict);
            if (!span_aa) continue;

            long ac = 0;
            t_atom *av = NULL;
            atomarray_getatoms(span_aa, &ac, &av);
            if (ac > 0) {
                // Use the first bar of the span as a unique identifier for the span
                t_atom_long first_bar = atom_getlong(av);
                char first_bar_str[64];
                snprintf(first_bar_str, 64, "%lld", (long long)first_bar);
                t_symbol *first_bar_sym = gensym(first_bar_str);

                if (!dictionary_hasentry(seen_spans, first_bar_sym)) {
                    dictionary_appendlong(seen_spans, first_bar_sym, 1);

                    double rating = 0.0;
                    t_atom r_atom;
                    if (dictionary_getatom(bar_dict, gensym("rating"), &r_atom) == MAX_ERR_NONE) {
                        rating = atom_getfloat(&r_atom);
                    }
                    crucible_visualize_state(x, gensym("new_span"), t_sym, span_aa, rating, 0);
                }
            }
            object_release((t_object *)span_aa);
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
        object_release((t_object *)seen_spans);
    }

    if (track_keys) sysmem_freeptr(track_keys);
    dictobj_release(incumbent_dict);
}

typedef struct {
    double *data;
    long count;
    long capacity;
} t_double_list;

void crucible_copy_key(t_dictionary *src_dict, t_dictionary *dest_dict, t_symbol *key) {
    t_atom value;
    if (dictionary_getatom(src_dict, key, &value) == MAX_ERR_NONE) {
        if (atom_gettype(&value) == A_OBJ) {
            t_object *obj = atom_getobj(&value);
            if (obj) {
                if (object_classname_compare(obj, gensym("dictionary"))) {
                    t_dictionary *nested_copy = dictionary_deep_copy((t_dictionary *)obj);
                    if (nested_copy) {
                        dictionary_appenddictionary(dest_dict, key, (t_object *)nested_copy);
                    }
                } else if (object_classname_compare(obj, gensym("atomarray"))) {
                    t_atomarray *aa_src = (t_atomarray *)obj;
                    long aa_len = 0;
                    t_atom *aa_atoms = NULL;
                    atomarray_getatoms(aa_src, &aa_len, &aa_atoms);
                    t_atomarray *aa_dest = atomarray_new(aa_len, aa_atoms);
                    if (aa_dest) {
                        dictionary_appendatomarray(dest_dict, key, (t_object *)aa_dest);
                    }
                }
            }
        } else {
            dictionary_appendatom(dest_dict, key, &value);
        }
    }
}

t_atom_long *crucible_get_span_list_longs(t_dictionary *bar_dict, long *out_count) {
    t_atomarray *span_aa = crucible_get_span_as_atomarray(bar_dict);
    if (!span_aa) {
        *out_count = 0;
        return NULL;
    }
    long ac = 0;
    t_atom *av = NULL;
    atomarray_getatoms(span_aa, &ac, &av);
    if (ac == 0) {
        object_release((t_object *)span_aa);
        *out_count = 0;
        return NULL;
    }
    t_atom_long *list = (t_atom_long *)sysmem_newptr(ac * sizeof(t_atom_long));
    for (long i = 0; i < ac; i++) {
        list[i] = atom_getlong(av + i);
    }
    *out_count = ac;
    object_release((t_object *)span_aa);
    return list;
}

void crucible_get_float_array(t_dictionary *bar_dict, t_symbol *key, double **out_array, long *out_count) {
    t_atomarray *aa = NULL;
    t_atom a;
    if (dictionary_getatomarray(bar_dict, key, (t_object **)&aa) == MAX_ERR_NONE && aa) {
        long ac = 0;
        t_atom *av = NULL;
        atomarray_getatoms(aa, &ac, &av);
        if (ac > 0) {
            double *arr = (double *)sysmem_newptr(ac * sizeof(double));
            for (long i = 0; i < ac; i++) {
                arr[i] = atom_getfloat(av + i);
            }
            *out_array = arr;
            *out_count = ac;
            return;
        }
    } else if (dictionary_getatom(bar_dict, key, &a) == MAX_ERR_NONE) {
        double *arr = (double *)sysmem_newptr(1 * sizeof(double));
        arr[0] = atom_getfloat(&a);
        *out_array = arr;
        *out_count = 1;
        return;
    }
    *out_array = NULL;
    *out_count = 0;
}

int crucible_compare_track_keys(const void *a, const void *b) {
    t_symbol *sa = *(t_symbol *const *)a;
    t_symbol *sb = *(t_symbol *const *)b;
    t_atom_long la = atoll(sa->s_name);
    t_atom_long lb = atoll(sb->s_name);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

int crucible_compare_bar_keys(const void *a, const void *b) {
    t_symbol *sa = *(t_symbol *const *)a;
    t_symbol *sb = *(t_symbol *const *)b;
    t_atom_long la = atoll(sa->s_name);
    t_atom_long lb = atoll(sb->s_name);
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

void crucible_do_rebar(t_crucible *x, t_atom_long new_bar_length) {
    t_atom_long old_bar_length = crucible_get_bar_length(x);
    if (old_bar_length <= 0) {
        old_bar_length = (x->local_bar_length > 0) ? (t_atom_long)x->local_bar_length : new_bar_length;
    }

    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) {
        object_error((t_object *)x, "rebar: could not find registered dictionary %s", x->incumbent_dict_name->s_name);
        return;
    }

    t_dictionary *new_incumbent_dict = dictionary_new();
    if (!new_incumbent_dict) {
        dictobj_release(incumbent_dict);
        return;
    }

    t_symbol **track_keys = NULL;
    long num_tracks = 0;
    dictionary_getkeys(incumbent_dict, &num_tracks, &track_keys);

    for (long i = 0; i < num_tracks; i++) {
        t_symbol *track_sym = track_keys[i];
        t_dictionary *track_dict = NULL;
        dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&track_dict);
        if (!track_dict) continue;

        t_dictionary *new_track_dict = dictionary_new();
        if (!new_track_dict) continue;

        t_symbol **bar_keys = NULL;
        long num_bars = 0;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        t_dictionary *processed_old_bars = dictionary_new();

        for (long j = 0; j < num_bars; j++) {
            t_symbol *bar_sym = bar_keys[j];
            if (dictionary_hasentry(processed_old_bars, bar_sym)) continue;

            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
            if (!bar_dict) continue;

            long pre_span_len = 0;
            t_atom_long *pre_span_ts = crucible_get_span_list_longs(bar_dict, &pre_span_len);
            if (pre_span_len == 0) {
                pre_span_len = 1;
                pre_span_ts = (t_atom_long *)sysmem_newptr(sizeof(t_atom_long));
                pre_span_ts[0] = atoll(bar_sym->s_name);
            }

            for (long k = 0; k < pre_span_len; k++) {
                char member_str[64];
                snprintf(member_str, 64, "%lld", (long long)pre_span_ts[k]);
                dictionary_appendlong(processed_old_bars, gensym(member_str), 1);
            }

            t_atom_long *new_ts_list = (t_atom_long *)sysmem_newptr(pre_span_len * sizeof(t_atom_long));
            long new_ts_count = 0;
            for (long k = 0; k < pre_span_len; k++) {
                t_atom_long old_ts = pre_span_ts[k];
                t_atom_long new_ts = (t_atom_long)round((double)old_ts / (double)new_bar_length) * new_bar_length;
                int dup = 0;
                for (long m = 0; m < new_ts_count; m++) {
                    if (new_ts_list[m] == new_ts) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    new_ts_list[new_ts_count++] = new_ts;
                }
            }

            qsort(new_ts_list, new_ts_count, sizeof(t_atom_long), crucible_compare_longs);

            t_atom_long lowest_new_ts = new_ts_list[0];
            t_atom_long old_highest = pre_span_ts[0];
            for (long m = 1; m < pre_span_len; m++) {
                if (pre_span_ts[m] > old_highest) {
                    old_highest = pre_span_ts[m];
                }
            }
            t_atom_long upper_bound = old_highest + old_bar_length;

            long new_span_len = 0;
            if (upper_bound > lowest_new_ts) {
                new_span_len = (upper_bound - 1 - lowest_new_ts) / new_bar_length + 1;
            }
            if (new_span_len < 1) {
                new_span_len = 1;
            }

            t_atom_long *new_assembled_span = (t_atom_long *)sysmem_newptr(new_span_len * sizeof(t_atom_long));
            for (long k = 0; k < new_span_len; k++) {
                new_assembled_span[k] = lowest_new_ts + k * new_bar_length;
            }
            t_atom_long highest_span_ts = new_assembled_span[new_span_len - 1];

            t_double_list *accum_scores = (t_double_list *)sysmem_newptr(new_span_len * sizeof(t_double_list));
            t_double_list *accum_absolutes = (t_double_list *)sysmem_newptr(new_span_len * sizeof(t_double_list));
            for (long k = 0; k < new_span_len; k++) {
                accum_scores[k].data = NULL;
                accum_scores[k].count = 0;
                accum_scores[k].capacity = 0;
                accum_absolutes[k].data = NULL;
                accum_absolutes[k].count = 0;
                accum_absolutes[k].capacity = 0;
            }

            for (long k = 0; k < pre_span_len; k++) {
                t_atom_long old_ts = pre_span_ts[k];
                char old_ts_str[64];
                snprintf(old_ts_str, 64, "%lld", (long long)old_ts);
                t_dictionary *old_bar_dict = NULL;
                dictionary_getdictionary(track_dict, gensym(old_ts_str), (t_object **)&old_bar_dict);
                if (!old_bar_dict) continue;

                double offset_val = 0.0;
                t_atom offset_atom;
                if (dictionary_getatom(old_bar_dict, gensym("offset"), &offset_atom) == MAX_ERR_NONE) {
                    offset_val = atom_getfloat(&offset_atom);
                }

                double *old_abs = NULL;
                long old_abs_len = 0;
                crucible_get_float_array(old_bar_dict, gensym("absolutes"), &old_abs, &old_abs_len);

                double *old_scores = NULL;
                long old_scores_len = 0;
                crucible_get_float_array(old_bar_dict, gensym("scores"), &old_scores, &old_scores_len);

                long num_pairs = (old_abs_len < old_scores_len) ? old_abs_len : old_scores_len;
                for (long p = 0; p < num_pairs; p++) {
                    double abs_val = old_abs[p];
                    double score_val = old_scores[p];
                    t_atom_long T_post = (t_atom_long)floor((abs_val - offset_val) / (double)new_bar_length) * new_bar_length;
                    if (T_post >= lowest_new_ts && T_post <= highest_span_ts) {
                        long idx = (T_post - lowest_new_ts) / new_bar_length;
                        t_double_list *sl = &accum_scores[idx];
                        if (sl->count >= sl->capacity) {
                            sl->capacity = sl->capacity == 0 ? 4 : sl->capacity * 2;
                            sl->data = (double *)sysmem_resizeptr(sl->data, sl->capacity * sizeof(double));
                        }
                        sl->data[sl->count++] = score_val;

                        t_double_list *al = &accum_absolutes[idx];
                        if (al->count >= al->capacity) {
                            al->capacity = al->capacity == 0 ? 4 : al->capacity * 2;
                            al->data = (double *)sysmem_resizeptr(al->data, al->capacity * sizeof(double));
                        }
                        al->data[al->count++] = abs_val;
                    } else {
                        object_warn((t_object *)x, "rebar: Pair forgotten. Track: %s, Pre-conversion bar: %lld, Absolute: %.6f, Score: %.6f, Offset: %.6f, Calculated post-conversion bar %lld is outside newly assembled span [%lld, %lld]", track_sym->s_name, (long long)old_ts, abs_val, score_val, offset_val, (long long)T_post, (long long)lowest_new_ts, (long long)highest_span_ts);
                    }
                }

                if (old_abs) sysmem_freeptr(old_abs);
                if (old_scores) sysmem_freeptr(old_scores);
            }

            double *new_means = (double *)sysmem_newptr(new_span_len * sizeof(double));
            int *mean_is_null = (int *)sysmem_newptr(new_span_len * sizeof(int));
            double min_mean = -1.0;
            int has_min_mean = 0;
            for (long k = 0; k < new_span_len; k++) {
                if (accum_scores[k].count == 0) {
                    mean_is_null[k] = 1;
                    new_means[k] = 0.0;
                } else {
                    mean_is_null[k] = 0;
                    double sum = 0.0;
                    for (long p = 0; p < accum_scores[k].count; p++) {
                        sum += accum_scores[k].data[p];
                    }
                    new_means[k] = sum / (double)accum_scores[k].count;
                }
                if (!mean_is_null[k]) {
                    if (!has_min_mean || new_means[k] < min_mean) {
                        min_mean = new_means[k];
                        has_min_mean = 1;
                    }
                }
            }

            double span_rating = 0.0;
            if (has_min_mean) {
                span_rating = min_mean * (double)new_span_len;
            }

            for (long k = 0; k < new_span_len; k++) {
                t_atom_long b_new = new_assembled_span[k];
                t_dictionary *new_bar_dict = dictionary_new();

                t_atom *span_atoms = (t_atom *)sysmem_newptr(new_span_len * sizeof(t_atom));
                for (long m = 0; m < new_span_len; m++) {
                    atom_setlong(span_atoms + m, new_assembled_span[m]);
                }
                t_atomarray *span_aa = atomarray_new(new_span_len, span_atoms);
                dictionary_appendatomarray(new_bar_dict, gensym("span"), (t_object *)span_aa);
                sysmem_freeptr(span_atoms);

                t_atom_long nearest_old_ts = pre_span_ts[0];
                t_atom_long min_diff = llabs(b_new - pre_span_ts[0]);
                for (long m = 1; m < pre_span_len; m++) {
                    t_atom_long diff = llabs(b_new - pre_span_ts[m]);
                    if (diff < min_diff) {
                        min_diff = diff;
                        nearest_old_ts = pre_span_ts[m];
                    }
                }
                char nearest_str[64];
                snprintf(nearest_str, 64, "%lld", (long long)nearest_old_ts);
                t_dictionary *nearest_old_bar_dict = NULL;
                dictionary_getdictionary(track_dict, gensym(nearest_str), (t_object **)&nearest_old_bar_dict);

                if (nearest_old_bar_dict) {
                    crucible_copy_key(nearest_old_bar_dict, new_bar_dict, gensym("offset"));
                    crucible_copy_key(nearest_old_bar_dict, new_bar_dict, gensym("palette"));
                }

                t_atom *abs_atoms = (t_atom *)sysmem_newptr((accum_absolutes[k].count == 0 ? 1 : accum_absolutes[k].count) * sizeof(t_atom));
                for (long p = 0; p < accum_absolutes[k].count; p++) {
                    atom_setfloat(abs_atoms + p, accum_absolutes[k].data[p]);
                }
                t_atomarray *abs_aa = atomarray_new(accum_absolutes[k].count, abs_atoms);
                dictionary_appendatomarray(new_bar_dict, gensym("absolutes"), (t_object *)abs_aa);
                sysmem_freeptr(abs_atoms);

                t_atom *scores_atoms = (t_atom *)sysmem_newptr((accum_scores[k].count == 0 ? 1 : accum_scores[k].count) * sizeof(t_atom));
                for (long p = 0; p < accum_scores[k].count; p++) {
                    atom_setfloat(scores_atoms + p, accum_scores[k].data[p]);
                }
                t_atomarray *scores_aa = atomarray_new(accum_scores[k].count, scores_atoms);
                dictionary_appendatomarray(new_bar_dict, gensym("scores"), (t_object *)scores_aa);
                sysmem_freeptr(scores_atoms);

                if (mean_is_null[k]) {
                    t_atom null_atom;
                    atom_setobj(&null_atom, NULL);
                    dictionary_appendatom(new_bar_dict, gensym("mean"), &null_atom);
                } else {
                    dictionary_appendfloat(new_bar_dict, gensym("mean"), new_means[k]);
                }
                dictionary_appendfloat(new_bar_dict, gensym("rating"), span_rating);

                char b_new_str[64];
                snprintf(b_new_str, 64, "%lld", (long long)b_new);
                dictionary_appenddictionary(new_track_dict, gensym(b_new_str), (t_object *)new_bar_dict);
            }

            sysmem_freeptr(pre_span_ts);
            sysmem_freeptr(new_ts_list);
            sysmem_freeptr(new_assembled_span);
            sysmem_freeptr(new_means);
            sysmem_freeptr(mean_is_null);
            for (long k = 0; k < new_span_len; k++) {
                if (accum_scores[k].data) sysmem_freeptr(accum_scores[k].data);
                if (accum_absolutes[k].data) sysmem_freeptr(accum_absolutes[k].data);
            }
            sysmem_freeptr(accum_scores);
            sysmem_freeptr(accum_absolutes);
        }

        t_dictionary *sorted_track_dict = dictionary_new();
        t_symbol **new_bar_keys = NULL;
        long num_new_bars = 0;
        dictionary_getkeys(new_track_dict, &num_new_bars, &new_bar_keys);
        if (num_new_bars > 0 && new_bar_keys) {
            qsort(new_bar_keys, num_new_bars, sizeof(t_symbol *), crucible_compare_bar_keys);
            dictionary_copyentries(new_track_dict, sorted_track_dict, new_bar_keys);
            sysmem_freeptr(new_bar_keys);
        }
        object_release((t_object *)new_track_dict);

        dictionary_appenddictionary(new_incumbent_dict, track_sym, (t_object *)sorted_track_dict);
        if (bar_keys) sysmem_freeptr(bar_keys);
        object_release((t_object *)processed_old_bars);
    }

    if (track_keys) sysmem_freeptr(track_keys);

    dictionary_clear(incumbent_dict);
    t_symbol **new_track_keys = NULL;
    long num_new_tracks = 0;
    dictionary_getkeys(new_incumbent_dict, &num_new_tracks, &new_track_keys);
    if (num_new_tracks > 0 && new_track_keys) {
        qsort(new_track_keys, num_new_tracks, sizeof(t_symbol *), crucible_compare_track_keys);
        dictionary_copyentries(new_incumbent_dict, incumbent_dict, new_track_keys);
        sysmem_freeptr(new_track_keys);
    }

    x->local_bar_length = (double)new_bar_length;
    crucible_recalculate_reaches(x);

    if (x->visualize) {
        crucible_visualize_repopulate(x);

        t_symbol **vis_track_keys = NULL;
        long vis_num_tracks = 0;
        dictionary_getkeys(new_incumbent_dict, &vis_num_tracks, &vis_track_keys);

        for (long i = 0; i < vis_num_tracks; i++) {
            t_symbol *t_sym = vis_track_keys[i];
            t_dictionary *t_dict = NULL;
            dictionary_getdictionary(new_incumbent_dict, t_sym, (t_object **)&t_dict);
            if (!t_dict) continue;

            t_symbol **vis_bar_keys = NULL;
            long vis_num_bars = 0;
            dictionary_getkeys(t_dict, &vis_num_bars, &vis_bar_keys);

            for (long j = 0; j < vis_num_bars; j++) {
                t_symbol *b_sym = vis_bar_keys[j];
                t_dictionary *b_dict = NULL;
                dictionary_getdictionary(t_dict, b_sym, (t_object **)&b_dict);
                if (!b_dict) continue;

                double rating = 0.0;
                t_atom r_atom;
                if (dictionary_getatom(b_dict, gensym("rating"), &r_atom) == MAX_ERR_NONE) {
                    rating = atom_getfloat(&r_atom);
                }

                char vis_msg[256];
                snprintf(vis_msg, 256, "{\"event\":\"replace\",\"track\":\"%s\",\"bar\":\"%s\",\"rating\":%.6f}", t_sym->s_name, b_sym->s_name, rating);
                visualize((t_object *)x, vis_msg);
            }
            if (vis_bar_keys) sysmem_freeptr(vis_bar_keys);
        }
        if (vis_track_keys) sysmem_freeptr(vis_track_keys);
    }

    object_release((t_object *)new_incumbent_dict);
    dictobj_release(incumbent_dict);
}

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->async && x->worker && !async_worker_is_worker_thread(x->worker)) {
        async_worker_enqueue(x->worker, x, (method)crucible_do_anything, s, argc, argv);
        return;
    }

    if (x->defer && !systhread_ismainthread()) {
        defer(x, (method)crucible_do_anything, s, (short)argc, argv);
        return;
    }

    crucible_do_anything(x, s, argc, argv);
}

void crucible_do_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->log) {
        char *val_str = crucible_atoms_to_string(argc, argv);
        crucible_log(x, "Received message: %s %s", s->s_name, val_str ? val_str : "");
        if (val_str) sysmem_freeptr(val_str);
    }

    if (s == gensym("rebar") && argc > 0) {
        t_atom_long new_bl = atom_getlong(argv);
        if (new_bl > 0) {
            crucible_do_rebar(x, new_bl);
        } else {
            object_error((t_object *)x, "rebar message requires a positive integer argument");
        }
        return;
    }

    if (s == gensym("clear")) {
        x->song_reach = 0;
        if (x->track_reaches_dict) {
            dictionary_clear(x->track_reaches_dict);
        }
        if (x->challenger_dict) {
            dictionary_clear(x->challenger_dict);
        }

        x->last_track_id = gensym("");
        x->local_bar_length = 0;
        x->bar_warn_sent = 0;
        crucible_log(x, "Internal state cleared.");

        if (x->visualize) {
            visualize((t_object *)x, "{\"tracks\":{}}");
        }
        return;
    }

    if (s == gensym("reaches")) {
        // Mandatory kick
        t_symbol *tmp = x->incumbent_dict_name;
        x->incumbent_dict_name = _sym_nothing;
        x->incumbent_dict_name = tmp;

        crucible_recalculate_reaches(x);
        if (x->visualize) {
            crucible_visualize_repopulate(x);
            crucible_visualize_dump_all_spans(x);
        }

        if (x->outlet_reach_int) {
            t_atom song_reach_atom;
            atom_setlong(&song_reach_atom, x->song_reach);
            if (!x->async || systhread_ismainthread()) {
                outlet_anything(x->outlet_reach_int, gensym("song"), 1, &song_reach_atom);
            } else {
                defer(x, (method)crucible_defer_output, gensym("reach_song"), 1, &song_reach_atom);
            }

            if (x->track_reaches_dict) {
                t_symbol **keys = NULL;
                long numkeys = 0;
                dictionary_getkeys(x->track_reaches_dict, &numkeys, &keys);
                for (long i = 0; i < numkeys; i++) {
                    t_symbol *track_id_sym = keys[i];
                    t_atom_long reach = 0;
                    dictionary_getlong(x->track_reaches_dict, track_id_sym, &reach);
                    t_atom reach_list[2];
                    atom_setlong(reach_list, (t_atom_long)atol(track_id_sym->s_name));
                    atom_setlong(reach_list + 1, reach);
                    if (!x->async || systhread_ismainthread()) {
                        outlet_list(x->outlet_reach_int, NULL, 2, reach_list);
                    } else {
                        defer(x, (method)crucible_defer_output, gensym("reach_list"), 2, reach_list);
                    }
                }
                if (keys) sysmem_freeptr(keys);
            }
        }
        return;
    }

    if (s == gensym("track") && argc > 0) {
        if (atom_gettype(argv) == A_LONG) {
            char track_id_str[64];
            snprintf(track_id_str, 64, "%lld", (long long)atom_getlong(argv));
            x->last_track_id = gensym(track_id_str);
        } else if (atom_gettype(argv) == A_SYM) {
            x->last_track_id = atom_getsym(argv);
        } else {
            return;
        }
        crucible_log(x, "Last track ID set to: %s", x->last_track_id->s_name);
        return;
    }

    if (s == gensym("span") && argc > 0) {
        if (x->last_track_id == _sym_nothing || x->last_track_id == gensym("")) {
            object_error((t_object *)x, "Received span message before track ID was set");
            return;
        }
        t_atomarray *span_aa = atomarray_new(argc, argv);
        crucible_process_span(x, x->last_track_id, span_aa);
        object_release((t_object *)span_aa);
        return;
    }

    if (s == gensym("replace") && argc >= 2 && x->visualize) {
        char *sel_str = NULL;
        if (atom_gettype(argv) == A_SYM) {
            sel_str = atom_getsym(argv)->s_name;
        }
        if (sel_str) {
            char *track = NULL;
            char *bar = NULL;
            char *key = NULL;
            if (parse_selector(sel_str, &track, &bar, &key)) {
                if (strcmp(key, "rating") == 0) {
                    double rating = atom_getfloat(argv + 1);
                    char msg[256];
                    snprintf(msg, 256, "{\"event\":\"replace\",\"track\":\"%s\",\"bar\":\"%s\",\"rating\":%.6f}", track, bar, rating);
                    visualize((t_object *)x, msg);
                }
                sysmem_freeptr(track);
                sysmem_freeptr(bar);
                sysmem_freeptr(key);
            }
        }
        return;
    }

    char *track_str = NULL;
    char *bar_str = NULL;
    char *key_str = NULL;

    if (parse_selector(s->s_name, &track_str, &bar_str, &key_str)) {
        t_symbol *track_sym = gensym(track_str);
        t_symbol *bar_sym = gensym(bar_str);
        t_symbol *key_sym = gensym(key_str);

        // Get or create track dictionary
        t_dictionary *track_dict = NULL;
        if (!dictionary_hasentry(x->challenger_dict, track_sym)) {
            track_dict = dictionary_new();
            if (track_dict) {
                dictionary_appenddictionary(x->challenger_dict, track_sym, (t_object *)track_dict);
                dictionary_getdictionary(x->challenger_dict, track_sym, (t_object **)&track_dict);
            }
        } else {
            dictionary_getdictionary(x->challenger_dict, track_sym, (t_object **)&track_dict);
        }

        if (track_dict) {
            // Get or create bar dictionary
            t_dictionary *bar_dict = NULL;
            if (!dictionary_hasentry(track_dict, bar_sym)) {
                bar_dict = dictionary_new();
                if (bar_dict) {
                    dictionary_appenddictionary(track_dict, bar_sym, (t_object *)bar_dict);
                    dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
                }
            } else {
                dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
            }

            // Add data to bar dictionary
            if (bar_dict) {
                t_atomarray *aa = atomarray_new(argc, argv);
                if (aa) {
                    dictionary_appendatomarray(bar_dict, key_sym, (t_object *)aa);
                }
            }
        }

        sysmem_freeptr(track_str);
        sysmem_freeptr(bar_str);
        sysmem_freeptr(key_str);
    } else {
        crucible_log(x, "Unparsable message selector: %s", s->s_name);
    }
}


void crucible_assist(t_crucible *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1: Primary messages (clear, rebar, track, span, reaches, replace, log, consume, fill, visualize, async). Also sets incumbent dictionary name."); break;
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

t_atomarray *crucible_get_span_as_atomarray(t_dictionary *bar_dict) {
    t_atomarray *span_aa = NULL;
    t_atom span_atom;

    if (dictionary_getatomarray(bar_dict, gensym("span"), (t_object **)&span_aa) == MAX_ERR_NONE && span_aa) {
        object_retain((t_object *)span_aa);
        return span_aa;
    } else if (dictionary_getatom(bar_dict, gensym("span"), &span_atom) == MAX_ERR_NONE) {
        return atomarray_new(1, &span_atom);
    }
    return NULL;
}

int crucible_span_has_loser(t_atomarray *span_aa, t_dictionary *defeated_dict) {
    if (!span_aa || !defeated_dict) return 0;
    long span_len = 0;
    t_atom *span_atoms = NULL;
    atomarray_getatoms(span_aa, &span_len, &span_atoms);
    for (long i = 0; i < span_len; i++) {
        t_atom_long ts = atom_getlong(span_atoms + i);
        char ts_str[64];
        snprintf(ts_str, 64, "%lld", (long long)ts);
        if (dictionary_hasentry(defeated_dict, gensym(ts_str))) {
            return 1;
        }
    }
    return 0;
}

static long json_append_atom_or_array(char *buffer, long offset, long buffer_size, t_dictionary *dict, t_symbol *key) {
    if (offset >= buffer_size - 1) return offset;
    t_atomarray *aa = NULL;
    t_atom a;
    if (dictionary_getatomarray(dict, key, (t_object **)&aa) == MAX_ERR_NONE && aa) {
        long ac = 0;
        t_atom *av = NULL;
        atomarray_getatoms(aa, &ac, &av);
        offset += snprintf(buffer + offset, buffer_size - offset, "[");
        for (long i = 0; i < ac; i++) {
            if (offset >= buffer_size - 1) break;
            if (atom_gettype(av + i) == A_FLOAT) {
                offset += snprintf(buffer + offset, buffer_size - offset, "%.6f", atom_getfloat(av + i));
            } else if (atom_gettype(av + i) == A_LONG) {
                offset += snprintf(buffer + offset, buffer_size - offset, "%lld", (long long)atom_getlong(av + i));
            }
            if (i < ac - 1 && offset < buffer_size - 1) offset += snprintf(buffer + offset, buffer_size - offset, ",");
        }
        if (offset < buffer_size - 1) offset += snprintf(buffer + offset, buffer_size - offset, "]");
    } else if (dictionary_getatom(dict, key, &a) == MAX_ERR_NONE) {
        if (atom_gettype(&a) == A_FLOAT) {
            offset += snprintf(buffer + offset, buffer_size - offset, "%.6f", atom_getfloat(&a));
        } else if (atom_gettype(&a) == A_LONG) {
            offset += snprintf(buffer + offset, buffer_size - offset, "%lld", (long long)atom_getlong(&a));
        }
    } else {
        offset += snprintf(buffer + offset, buffer_size - offset, "null");
    }
    return (offset < buffer_size) ? offset : buffer_size - 1;
}

void crucible_visualize_state(t_crucible *x, t_symbol *event_type, t_symbol *track_id_sym, t_atomarray *span_aa, double rating, int include_tracks) {
    if (!x->visualize) return;

    t_dictionary *incumbent_dict = dictobj_findregistered_retain(x->incumbent_dict_name);
    if (!incumbent_dict) return;

    long buffer_size = 262144;
    char *json_buffer = (char *)sysmem_newptr(buffer_size);
    if (!json_buffer) {
        dictobj_release(incumbent_dict);
        return;
    }
    long offset = 0;

    t_atom_long bar_length = crucible_get_bar_length(x);

    offset += snprintf(json_buffer + offset, buffer_size - offset, "{\"bar_length\":%lld", (long long)bar_length);

    if (event_type && event_type != _sym_nothing) {
        if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"event\":\"%s\"", event_type->s_name);
        if (track_id_sym && offset < buffer_size - 1) {
            offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"new_span_track\":\"%s\"", track_id_sym->s_name);
        }
        if (span_aa && offset < buffer_size - 1) {
            long ac = 0;
            t_atom *av = NULL;
            atomarray_getatoms(span_aa, &ac, &av);
            offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"new_span_bars\":[");
            for (long i = 0; i < ac; i++) {
                if (offset >= buffer_size - 1) break;
                offset += snprintf(json_buffer + offset, buffer_size - offset, "%lld%s", (long long)atom_getlong(av + i), (i < ac - 1) ? "," : "");
            }
            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "]");

            if (track_id_sym && offset < buffer_size - 1) {
                t_dictionary *track_dict = NULL;
                if (dictionary_getdictionary(incumbent_dict, track_id_sym, (t_object **)&track_dict) == MAX_ERR_NONE && track_dict) {
                    offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"new_span_data\":{");
                    int first_data = 1;
                    for (long i = 0; i < ac; i++) {
                        if (offset >= buffer_size - 1) break;
                        t_atom_long b_ts = atom_getlong(av + i);
                        char b_ts_str[64];
                        snprintf(b_ts_str, 64, "%lld", (long long)b_ts);
                        t_dictionary *bar_dict = NULL;
                        if (dictionary_getdictionary(track_dict, gensym(b_ts_str), (t_object **)&bar_dict) == MAX_ERR_NONE && bar_dict) {
                            if (!first_data && offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                            first_data = 0;
                            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "\"%lld\":{\"absolutes\":", (long long)b_ts);
                            offset = json_append_atom_or_array(json_buffer, offset, buffer_size, bar_dict, gensym("absolutes"));
                            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"scores\":");
                            offset = json_append_atom_or_array(json_buffer, offset, buffer_size, bar_dict, gensym("scores"));
                            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"offset\":");
                            offset = json_append_atom_or_array(json_buffer, offset, buffer_size, bar_dict, gensym("offset"));
                            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "}");
                        }
                    }
                    if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "}");
                }
            }
        }
        if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"new_span_rating\":%.4f", rating);
    }

    if (include_tracks && offset < buffer_size - 1) {
        offset += snprintf(json_buffer + offset, buffer_size - offset, ",\"tracks\":{");

        t_symbol **track_keys = NULL;
        long num_tracks = 0;
        dictionary_getkeys(incumbent_dict, &num_tracks, &track_keys);

        int first_track = 1;
        for (long i = 0; i < num_tracks; i++) {
            if (offset >= buffer_size - 1) break;
            t_symbol *t_sym = track_keys[i];
            t_dictionary *track_dict = NULL;
            if (dictionary_getdictionary(incumbent_dict, t_sym, (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) continue;

            if (!first_track && offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
            first_track = 0;
            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "\"%s\":[", t_sym->s_name);

            t_symbol **bar_keys = NULL;
            long num_bars = 0;
            dictionary_getkeys(track_dict, &num_bars, &bar_keys);

            for (long j = 0; j < num_bars; j++) {
                if (offset >= buffer_size - 1) break;
                if (j > 0 && offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                // Check if it's a numeric bar key
                const char *bk = bar_keys[j]->s_name;
                int is_num = (bk && bk[0] != '\0');
                if (is_num) {
                    for(int k=0; bk[k]; k++) {
                        if(!isdigit(bk[k])) {
                            is_num = 0;
                            break;
                        }
                    }
                }

                if (is_num) {
                    offset += snprintf(json_buffer + offset, buffer_size - offset, "%s", bk);
                } else {
                    offset += snprintf(json_buffer + offset, buffer_size - offset, "\"%s\"", bk);
                }
            }

            if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "]");
            if (bar_keys) sysmem_freeptr(bar_keys);
        }
        if (track_keys) sysmem_freeptr(track_keys);
        if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "}");
    }

    if (offset < buffer_size - 1) offset += snprintf(json_buffer + offset, buffer_size - offset, "}");

    visualize((t_object *)x, json_buffer);
    sysmem_freeptr(json_buffer);
    dictobj_release(incumbent_dict);
}

t_max_err crucible_attr_set_visualize(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        long val = atom_getlong(av);
        x->visualize = val;
        if (val) {
            crucible_visualize_repopulate(x);
        }
    }
    return MAX_ERR_NONE;
}

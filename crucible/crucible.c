#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "../shared/logging.h"
#include <string.h>

typedef struct _crucible {
    t_object s_obj;
    t_dictionary *challenger_dict;
    t_dictionary *span_tracker_dict;
    t_symbol *incumbent_dict_name;
    void *outlet_data;
    void *outlet_fill;
    void *outlet_reach_int;
    void *log_outlet;
    t_buffer_ref *buffer_ref;
    long log;
    t_atom_long song_reach;
    double local_bar_length;
    long instance_id;
} t_crucible;

// Function prototypes
void *crucible_new(t_symbol *s, long argc, t_atom *argv);
void crucible_free(t_crucible *x);
void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_log(t_crucible *x, const char *fmt, ...);
char *crucible_atoms_to_string(long argc, t_atom *argv);
int parse_selector(const char *selector_str, char **track, char **bar, char **key);
t_dictionary *dictionary_deep_copy(t_dictionary *src);
void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, t_atom_long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict);
void crucible_local_bar_length(t_crucible *x, double f);
t_atom_long crucible_get_bar_length(t_crucible *x);


t_class *crucible_class;

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

void ext_main(void *r) {
    common_symbols_init();
    t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, sizeof(t_crucible), 0L, A_GIMME, 0);
    class_addmethod(c, (method)crucible_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)crucible_local_bar_length, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)crucible_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_crucible, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    crucible_class = c;
}

void *crucible_new(t_symbol *s, long argc, t_atom *argv) {
    t_crucible *x = (t_crucible *)object_alloc(crucible_class);
    if (x) {
        x->challenger_dict = dictionary_new();
        x->span_tracker_dict = dictionary_new();
        x->incumbent_dict_name = gensym("");
        x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        if (!buffer_ref_getobject(x->buffer_ref)) {
            object_error((t_object *)x, "bar buffer~ not found");
        }
        x->song_reach = 0;
        x->local_bar_length = 0;
        x->instance_id = 1000 + (rand() % 9000);

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->incumbent_dict_name = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        // Outlets are created from right to left
        if (x->log) {
            x->log_outlet = outlet_new((t_object *)x, NULL);
        }
        x->outlet_reach_int = outlet_new((t_object *)x, NULL);   // Index 2
        x->outlet_fill = outlet_new((t_object *)x, NULL);        // Index 1
        x->outlet_data = outlet_new((t_object *)x, NULL);        // Index 0

        floatin((t_object *)x, 1);
    }
    return (x);
}

void crucible_free(t_crucible *x) {
    if (x->challenger_dict) {
        object_release((t_object *)x->challenger_dict);
    }
    if (x->span_tracker_dict) {
        object_release((t_object *)x->span_tracker_dict);
    }
    if (x->buffer_ref) {
        object_free(x->buffer_ref);
    }
}

void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, t_atom_long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict) {
    if (!bar_dict) return;
    t_atomarray *offset_atomarray = NULL, *palette_atomarray = NULL, *bar_span_atomarray = NULL;
    dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_atomarray);
    dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_atomarray);
    dictionary_getatomarray(bar_dict, gensym("span"), (t_object **)&bar_span_atomarray);

    // Right-to-Left execution order: Reach, Offset, Bar, Track, Palette

    // 1. Reach
    if (bar_span_atomarray) {
        long bar_span_len = 0;
        t_atom *bar_span_atoms = NULL;
        atomarray_getatoms(bar_span_atomarray, &bar_span_len, &bar_span_atoms);

        t_atom_long max_val = 0;
        for (long j = 0; j < bar_span_len; j++) {
            t_atom_long current_val = atom_getlong(bar_span_atoms + j);
            if (current_val > max_val) max_val = current_val;
        }

        t_atom_long bar_length = crucible_get_bar_length(x);
        t_atom_long current_reach = max_val + bar_length;

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
                outlet_anything(x->outlet_data, gensym("-"), 3, reach_list);
            }
        }
    }

    // 2. Data List: [palette, track, bar, offset]
    t_atom list[4];
    t_symbol *palette_sym = _sym_nothing;
    double offset_val = 0.0;

    if (palette_atomarray) {
        long len;
        t_atom *atoms;
        atomarray_getatoms(palette_atomarray, &len, &atoms);
        if (len > 0 && atom_gettype(atoms) == A_SYM) palette_sym = atom_getsym(atoms);
    }

    if (offset_atomarray) {
        long len;
        t_atom *atoms;
        atomarray_getatoms(offset_atomarray, &len, &atoms);
        if (len > 0) {
            if (atom_gettype(atoms) == A_FLOAT) {
                offset_val = atom_getfloat(atoms);
            } else if (atom_gettype(atoms) == A_LONG) {
                offset_val = (double)atom_getlong(atoms);
            }
        }
    }

    atom_setsym(list, palette_sym);
    atom_setlong(list + 1, (t_atom_long)atol(track_sym->s_name));
    atom_setlong(list + 2, bar_ts_long);
    atom_setfloat(list + 3, offset_val);

    if (x->outlet_data) {
        outlet_list(x->outlet_data, NULL, 4, list);
    }
}

void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray) {
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

    // Get challenger track dictionary
    t_dictionary *challenger_track_dict = NULL;
    dictionary_getdictionary(x->challenger_dict, track_sym, (t_object **)&challenger_track_dict);
    if (!challenger_track_dict) {
        object_error((t_object *)x, "Could not find challenger track dict for %s", track_sym->s_name);
        return;
    }

    for (long i = 0; i < span_len; i++) {
        long bar_ts_long = atom_getlong(&span_atoms[i]);
        char bar_ts_str[32];
        snprintf(bar_ts_str, 32, "%ld", bar_ts_long);
        t_symbol *bar_sym = gensym(bar_ts_str);

        // Get challenger bar dictionary
        t_dictionary *challenger_bar_dict = NULL;
        dictionary_getdictionary(challenger_track_dict, bar_sym, (t_object **)&challenger_bar_dict);
        if (!challenger_bar_dict) continue;

        t_atomarray *challenger_rating_atomarray = NULL;
        dictionary_getatomarray(challenger_bar_dict, gensym("rating"), (t_object **)&challenger_rating_atomarray);
        if (!challenger_rating_atomarray) continue;

        long challenger_rating_len = 0;
        t_atom *challenger_rating_atoms = NULL;
        atomarray_getatoms(challenger_rating_atomarray, &challenger_rating_len, &challenger_rating_atoms);
        if (challenger_rating_len == 0) continue;
        double challenger_rating = atom_getfloat(challenger_rating_atoms);


        // Check against incumbent
        t_dictionary *incumbent_track_dict = NULL;
        if (dictionary_hasentry(incumbent_dict, track_sym)) {
            dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict);
        }

        if (incumbent_track_dict && dictionary_hasentry(incumbent_track_dict, bar_sym)) {
            t_dictionary *incumbent_bar_dict = NULL;
            dictionary_getdictionary(incumbent_track_dict, bar_sym, (t_object **)&incumbent_bar_dict);

            t_atomarray *incumbent_rating_atomarray = NULL;
            dictionary_getatomarray(incumbent_bar_dict, gensym("rating"), (t_object **)&incumbent_rating_atomarray);

            if (!incumbent_rating_atomarray) {
                crucible_log(x, "Bar %lld: Challenger rating %.2f vs Incumbent (no-contest, missing rating). Challenger wins bar.", (long long)bar_ts_long, challenger_rating);
                continue;
            }

            long incumbent_rating_len = 0;
            t_atom *incumbent_rating_atoms = NULL;
            atomarray_getatoms(incumbent_rating_atomarray, &incumbent_rating_len, &incumbent_rating_atoms);
            if(incumbent_rating_len == 0) {
                crucible_log(x, "Bar %lld: Challenger rating %.2f vs Incumbent (no-contest, empty atomarray). Challenger wins bar.", (long long)bar_ts_long, challenger_rating);
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
            crucible_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, no entry). Challenger wins bar.", bar_ts_long, challenger_rating);
        }
    }

    if (challenger_wins) {
        crucible_log(x, "Challenger span for track %s won. Overwriting incumbent dictionary.", track_sym->s_name);

        t_atom_long max_reach = 0;
        for (long i = 0; i < span_len; i++) {
            t_atom_long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[64];
            snprintf(bar_ts_str, 64, "%lld", (long long)bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            t_dictionary *challenger_bar_dict = NULL;
            dictionary_getdictionary(challenger_track_dict, bar_sym, (t_object **)&challenger_bar_dict);
            if (!challenger_bar_dict) continue;

            t_atomarray *bar_span_atomarray = NULL;
            dictionary_getatomarray(challenger_bar_dict, gensym("span"), (t_object **)&bar_span_atomarray);

            if (bar_span_atomarray) {
                long bar_span_len = 0;
                t_atom *bar_span_atoms = NULL;
                atomarray_getatoms(bar_span_atomarray, &bar_span_len, &bar_span_atoms);

                t_atom_long max_val = 0;
                for (long j = 0; j < bar_span_len; j++) {
                    t_atom_long current_val = atom_getlong(bar_span_atoms + j);
                    if (current_val > max_val) max_val = current_val;
                }

                t_atom_long bar_length = crucible_get_bar_length(x);
                t_atom_long current_reach = max_val + bar_length;
                if (current_reach > max_reach) {
                    max_reach = current_reach;
                }
            }
        }

        // Get or create incumbent track dictionary
        t_dictionary *incumbent_track_dict = NULL;
        if (!dictionary_hasentry(incumbent_dict, track_sym)) {
            incumbent_track_dict = dictionary_new();
            dictionary_appenddictionary(incumbent_dict, track_sym, (t_object *)incumbent_track_dict);
        } else {
            dictionary_getdictionary(incumbent_dict, track_sym, (t_object **)&incumbent_track_dict);
        }

        int song_grew = (max_reach > x->song_reach);
        t_atom_long old_song_reach = x->song_reach;
        if (song_grew) {
            x->song_reach = max_reach;
            crucible_log(x, "Song has grown. New reach is %lld (previously %lld).", (long long)x->song_reach, (long long)old_song_reach);
        }

        for (long i = 0; i < span_len; i++) {
            long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[32];
            snprintf(bar_ts_str, 32, "%ld", bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            t_dictionary *challenger_bar_dict = NULL;
            dictionary_getdictionary(challenger_track_dict, bar_sym, (t_object **)&challenger_bar_dict);

            if (challenger_bar_dict) {
                if (incumbent_track_dict && dictionary_hasentry(incumbent_track_dict, bar_sym)) {
                     dictionary_deleteentry(incumbent_track_dict, bar_sym);
                }
                t_dictionary *copied_bar_dict = dictionary_deep_copy(challenger_bar_dict);
                if (copied_bar_dict && incumbent_track_dict) {
                    dictionary_appenddictionary(incumbent_track_dict, bar_sym, (t_object *)copied_bar_dict);
                    crucible_log(x, "  -> Wrote bar %s to incumbent track %s", bar_sym->s_name, track_sym->s_name);
                }
            }
        }

        // Standard Max right-to-left outlet firing order:
        // Reach Int (Index 2) -> Fill (Index 1) -> Data (Index 0)
        if (song_grew) {
            if (x->outlet_reach_int) {
                outlet_int(x->outlet_reach_int, (t_atom_long)x->song_reach);
            }
            if (x->outlet_fill) {
                outlet_anything(x->outlet_fill, gensym("fill"), 0, NULL);
            }
        }

        for (long i = 0; i < span_len; i++) {
            long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[32];
            snprintf(bar_ts_str, 32, "%ld", bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(incumbent_track_dict, bar_sym, (t_object **)&bar_dict);
            if (bar_dict) {
                crucible_output_bar_data(x, bar_dict, bar_ts_long, track_sym, incumbent_track_dict);
            }
        }
    } else {
        crucible_log(x, "Challenger span for track %s lost.", track_sym->s_name);
    }

    // Cleanup challenger dict for this track
    if (dictionary_hasentry(x->challenger_dict, track_sym)) {
        dictionary_deleteentry(x->challenger_dict, track_sym);
        crucible_log(x, "Cleaned up challenger data for track %s.", track_sym->s_name);
    }

    object_release((t_object *)incumbent_dict);
}

t_atom_long crucible_get_bar_length(t_crucible *x) {
    if (x->local_bar_length > 0) {
        return (t_atom_long)x->local_bar_length;
    }

    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        object_error((t_object *)x, "bar buffer~ not found");
        return 0;
    }

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
        x->local_bar_length = (double)bar_length;
        crucible_log(x, "thread %ld: bar_length changed to %lld", x->instance_id, (long long)bar_length);
    }

    return bar_length;
}

void crucible_local_bar_length(t_crucible *x, double f) {
    if (f <= 0) {
        x->local_bar_length = 0;
    } else {
        x->local_bar_length = f;
    }
    crucible_log(x, "thread %ld: bar_length changed to %lld", x->instance_id, (long long)x->local_bar_length);
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

void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv) {
    if (x->log) {
        char *val_str = crucible_atoms_to_string(argc, argv);
        crucible_log(x, "Received message: %s %s", s->s_name, val_str ? val_str : "");
        if (val_str) sysmem_freeptr(val_str);
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
            dictionary_appenddictionary(x->challenger_dict, track_sym, (t_object *)track_dict);
        } else {
            dictionary_getdictionary(x->challenger_dict, track_sym, (t_object **)&track_dict);
        }

        // Get or create bar dictionary
        t_dictionary *bar_dict = NULL;
        if (!dictionary_hasentry(track_dict, bar_sym)) {
            bar_dict = dictionary_new();
            dictionary_appenddictionary(track_dict, bar_sym, (t_object *)bar_dict);
        } else {
            dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
        }

        // Add data to bar dictionary
        dictionary_appendatomarray(bar_dict, key_sym, (t_object *)atomarray_new(argc, argv));

        // Use span_tracker_dict to know when a span is complete
        char received_keys_key_str[256];
        snprintf(received_keys_key_str, 256, "%s::received_keys", track_sym->s_name);
        t_symbol *received_keys_sym = gensym(received_keys_key_str);

        t_atom_long received_keys = 0;
        if (dictionary_hasentry(x->span_tracker_dict, received_keys_sym)) {
            dictionary_getlong(x->span_tracker_dict, received_keys_sym, &received_keys);
        }
        received_keys++;
        dictionary_appendlong(x->span_tracker_dict, received_keys_sym, received_keys);

        if (strcmp(key_str, "span") == 0) {
            t_atom_long num_bars = (t_atom_long)argc;
            t_atom_long expected_keys = num_bars * 7;
            char expected_keys_key_str[256];
            snprintf(expected_keys_key_str, 256, "%s::expected_keys", track_sym->s_name);
            dictionary_appendlong(x->span_tracker_dict, gensym(expected_keys_key_str), expected_keys);

            char span_bars_key_str[256];
            snprintf(span_bars_key_str, 256, "%s::span_bars", track_sym->s_name);
            dictionary_appendatomarray(x->span_tracker_dict, gensym(span_bars_key_str), (t_object *)atomarray_new(argc, argv));
        }

        char expected_keys_key_str[256];
        snprintf(expected_keys_key_str, 256, "%s::expected_keys", track_sym->s_name);
        t_symbol *expected_keys_sym = gensym(expected_keys_key_str);

        if (dictionary_hasentry(x->span_tracker_dict, expected_keys_sym)) {
            t_atom_long expected_keys = 0;
            dictionary_getlong(x->span_tracker_dict, expected_keys_sym, &expected_keys);

            if (received_keys >= expected_keys) {
                char span_bars_key_str[256];
                snprintf(span_bars_key_str, 256, "%s::span_bars", track_sym->s_name);
                t_symbol *span_bars_sym = gensym(span_bars_key_str);

                t_atomarray *span_atomarray = NULL;
                dictionary_getatomarray(x->span_tracker_dict, span_bars_sym, (t_object **)&span_atomarray);
                if (span_atomarray) {
                    crucible_process_span(x, track_sym, span_atomarray);
                }

                // Clean up tracker dict for this track
                dictionary_deleteentry(x->span_tracker_dict, received_keys_sym);
                dictionary_deleteentry(x->span_tracker_dict, expected_keys_sym);
                dictionary_deleteentry(x->span_tracker_dict, span_bars_sym);
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
            case 0: sprintf(s, "Inlet 1: (anything) Message Stream from buildspans, (symbol) Incumbent Dictionary Name"); break;
            case 1: sprintf(s, "Inlet 2: (float) Local Bar Length"); break;
        }
    } else { // ASSIST_OUTLET
        if (x->log) {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: Data and Reach Lists"); break;
                case 1: sprintf(s, "Outlet 2: Fill (symbol)"); break;
                case 2: sprintf(s, "Outlet 3: Reach (int)"); break;
                case 3: sprintf(s, "Outlet 4: Logging Outlet"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: Data and Reach Lists"); break;
                case 1: sprintf(s, "Outlet 2: Fill (symbol)"); break;
                case 2: sprintf(s, "Outlet 3: Reach (int)"); break;
            }
        }
    }
}

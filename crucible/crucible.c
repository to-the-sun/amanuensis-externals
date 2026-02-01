#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include <string.h>

typedef struct _crucible {
    t_object s_obj;
    t_dictionary *challenger_dict;
    t_dictionary *span_tracker_dict;
    t_symbol *incumbent_dict_name;
    void *outlet_reach;
    void *outlet_fill;
    void *outlet_palette;
    void *outlet_track;
    void *outlet_bar;
    void *outlet_offset;
    void *verbose_log_outlet;
    t_buffer_ref *buffer_ref;
    long verbose;
    long fill;
    long song_reach;
    double local_bar_length;
    long instance_id;
} t_crucible;

// Function prototypes
void *crucible_new(t_symbol *s, long argc, t_atom *argv);
void crucible_free(t_crucible *x);
void crucible_anything(t_crucible *x, t_symbol *s, long argc, t_atom *argv);
void crucible_process_span(t_crucible *x, t_symbol *track_sym, t_atomarray *span_atomarray);
void crucible_assist(t_crucible *x, void *b, long m, long a, char *s);
void crucible_verbose_log(t_crucible *x, const char *fmt, ...);
char *crucible_atoms_to_string(long argc, t_atom *argv);
int parse_selector(const char *selector_str, char **track, char **bar, char **key);
t_dictionary *dictionary_deep_copy(t_dictionary *src);
void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict);
void crucible_local_bar_length(t_crucible *x, double f);
long crucible_get_bar_length(t_crucible *x);


t_class *crucible_class;

void crucible_verbose_log(t_crucible *x, const char *fmt, ...) {
    if (x->verbose && x->verbose_log_outlet) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        outlet_anything(x->verbose_log_outlet, gensym(buf), 0, NULL);
    }
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
    t_class *c;
    c = class_new("crucible", (method)crucible_new, (method)crucible_free, (short)sizeof(t_crucible), 0L, A_GIMME, 0);
    class_addmethod(c, (method)crucible_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)crucible_local_bar_length, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)crucible_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_crucible, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");

    CLASS_ATTR_LONG(c, "fill", 0, t_crucible, fill);
    CLASS_ATTR_STYLE_LABEL(c, "fill", 0, "onoff", "Enable Song Fill");

    class_register(CLASS_BOX, c);
    crucible_class = c;
}

void *crucible_new(t_symbol *s, long argc, t_atom *argv) {
    t_crucible *x = (t_crucible *)object_alloc(crucible_class);
    if (x) {
        x->challenger_dict = dictionary_new();
        x->span_tracker_dict = dictionary_new();
        x->verbose_log_outlet = NULL;
        x->incumbent_dict_name = gensym("");
        x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        x->song_reach = 0;
        x->local_bar_length = 0;
        x->instance_id = 1000 + (rand() % 9000);

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->incumbent_dict_name = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        if (x->verbose) {
            x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        }
        x->outlet_fill = outlet_new((t_object *)x, NULL);
        x->outlet_reach = outlet_new((t_object *)x, NULL);
        x->outlet_offset = outlet_new((t_object *)x, NULL);
        x->outlet_bar = outlet_new((t_object *)x, NULL);
        x->outlet_track = outlet_new((t_object *)x, NULL);
        x->outlet_palette = outlet_new((t_object *)x, NULL);

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

void crucible_output_bar_data(t_crucible *x, t_dictionary *bar_dict, long bar_ts_long, t_symbol *track_sym, t_dictionary *incumbent_track_dict) {
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

        long max_val = 0;
        for (long j = 0; j < bar_span_len; j++) {
            long current_val = atom_getlong(bar_span_atoms + j);
            if (current_val > max_val) max_val = current_val;
        }

        long bar_length = crucible_get_bar_length(x);
        long current_reach = max_val + bar_length;

        char reach_str[32];
        snprintf(reach_str, 32, "%ld", current_reach);

        crucible_verbose_log(x, "Checking reach %ld for track %s", current_reach, track_sym->s_name);
        if (incumbent_track_dict && !dictionary_hasentry(incumbent_track_dict, gensym(reach_str))) {
            crucible_verbose_log(x, "  -> Reach %ld not found in incumbent. Sending reach message.", current_reach);
            t_atom reach_list[3];
            atom_setlong(reach_list, atol(track_sym->s_name));
            atom_setlong(reach_list + 1, current_reach);
            atom_setfloat(reach_list + 2, -999999.0);
            outlet_anything(x->outlet_reach, gensym("-"), 3, reach_list);
        } else {
            if (incumbent_track_dict) {
                crucible_verbose_log(x, "  -> Reach %ld already exists in incumbent. Suppressing reach message.", current_reach);
            } else {
                crucible_verbose_log(x, "  -> No incumbent track dict. Suppressing reach message.");
            }
        }
    }

    // 2. Offset
    if (offset_atomarray) {
        long len;
        t_atom *atoms;
        atomarray_getatoms(offset_atomarray, &len, &atoms);
        if (len > 0) {
            if (atom_gettype(atoms) == A_FLOAT) {
                outlet_float(x->outlet_offset, atom_getfloat(atoms));
            } else {
                outlet_int(x->outlet_offset, atom_getlong(atoms));
            }
        }
    }

    // 3. Bar
    outlet_int(x->outlet_bar, bar_ts_long);

    // 4. Track
    outlet_int(x->outlet_track, atol(track_sym->s_name));

    // 5. Palette
    if (palette_atomarray) {
        long len;
        t_atom *atoms;
        atomarray_getatoms(palette_atomarray, &len, &atoms);
        if (len > 0) outlet_anything(x->outlet_palette, atom_getsym(atoms), 0, NULL);
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

            long incumbent_rating_len = 0;
            t_atom *incumbent_rating_atoms = NULL;
            atomarray_getatoms(incumbent_rating_atomarray, &incumbent_rating_len, &incumbent_rating_atoms);
            if(incumbent_rating_len == 0) {
                crucible_verbose_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, empty atomarray). Challenger wins bar.", bar_ts_long, challenger_rating);
                continue;
            }

            double incumbent_rating = atom_getfloat(incumbent_rating_atoms);
            crucible_verbose_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent rating %.2f.", bar_ts_long, challenger_rating, incumbent_rating);
            if (challenger_rating <= incumbent_rating) {
                crucible_verbose_log(x, "-> Challenger loses bar. Span comparison failed.");
                challenger_wins = 0;
                break;
            } else {
                crucible_verbose_log(x, "-> Challenger wins bar.");
            }
        } else {
            crucible_verbose_log(x, "Bar %ld: Challenger rating %.2f vs Incumbent (no-contest, no entry). Challenger wins bar.", bar_ts_long, challenger_rating);
        }
    }

    if (challenger_wins) {
        crucible_verbose_log(x, "Challenger span for track %s won. Overwriting incumbent dictionary.", track_sym->s_name);

        long max_reach = 0;
        for (long i = 0; i < span_len; i++) {
            long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[32];
            snprintf(bar_ts_str, 32, "%ld", bar_ts_long);
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

                long max_val = 0;
                for (long j = 0; j < bar_span_len; j++) {
                    long current_val = atom_getlong(bar_span_atoms + j);
                    if (current_val > max_val) max_val = current_val;
                }

                long bar_length = crucible_get_bar_length(x);
                long current_reach = max_val + bar_length;
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

        for (long i = 0; i < span_len; i++) {
            long bar_ts_long = atom_getlong(&span_atoms[i]);
            char bar_ts_str[32];
            snprintf(bar_ts_str, 32, "%ld", bar_ts_long);
            t_symbol *bar_sym = gensym(bar_ts_str);

            t_dictionary *challenger_bar_dict = NULL;
            dictionary_getdictionary(challenger_track_dict, bar_sym, (t_object **)&challenger_bar_dict);

            if (challenger_bar_dict) {
                if (dictionary_hasentry(incumbent_track_dict, bar_sym)) {
                     dictionary_deleteentry(incumbent_track_dict, bar_sym);
                }
                t_dictionary *copied_bar_dict = dictionary_deep_copy(challenger_bar_dict);
                dictionary_appenddictionary(incumbent_track_dict, bar_sym, (t_object *)copied_bar_dict);
                crucible_verbose_log(x, "  -> Wrote bar %s to incumbent track %s", bar_sym->s_name, track_sym->s_name);
                crucible_output_bar_data(x, copied_bar_dict, bar_ts_long, track_sym, incumbent_track_dict);
            }
        }
        if (max_reach > x->song_reach) {
            long old_song_reach = x->song_reach;
            x->song_reach = max_reach;
            crucible_verbose_log(x, "Song has grown. New reach is %ld (previously %ld).", x->song_reach, old_song_reach);

            outlet_anything(x->outlet_fill, gensym("fill"), 0, NULL);

            if (x->fill && old_song_reach > 0) {
                t_symbol **track_keys = NULL;
                long num_tracks = 0;
                dictionary_getkeys(incumbent_dict, &num_tracks, &track_keys);

                for (long i = 0; i < num_tracks; i++) {
                    if (track_keys[i] != track_sym) { // Not the track that grew the song
                        t_dictionary *other_track_dict = NULL;
                        dictionary_getdictionary(incumbent_dict, track_keys[i], (t_object **)&other_track_dict);

                        if (other_track_dict) {
                            for (long t = old_song_reach; t < x->song_reach; t++) {
                                long source_ts = t % old_song_reach;
                                char source_ts_str[32];
                                snprintf(source_ts_str, 32, "%ld", source_ts);
                                t_symbol *source_ts_sym = gensym(source_ts_str);

                                if (dictionary_hasentry(other_track_dict, source_ts_sym)) {
                                    t_dictionary *source_bar_dict = NULL;
                                    dictionary_getdictionary(other_track_dict, source_ts_sym, (t_object **)&source_bar_dict);
                                    if (source_bar_dict) {
                                        char target_ts_str[32];
                                        snprintf(target_ts_str, 32, "%ld", t);
                                        t_symbol *target_ts_sym = gensym(target_ts_str);

                                        if (dictionary_hasentry(other_track_dict, target_ts_sym)) {
                                            dictionary_deleteentry(other_track_dict, target_ts_sym);
                                        }
                                        t_dictionary *copied_bar_dict = dictionary_deep_copy(source_bar_dict);

                                        // Adjust absolutes in the copied bar
                                        t_atomarray *absolutes_atomarray = NULL;
                                        if (dictionary_hasentry(copied_bar_dict, gensym("absolutes"))) {
                                            dictionary_getatomarray(copied_bar_dict, gensym("absolutes"), (t_object **)&absolutes_atomarray);
                                            if (absolutes_atomarray) {
                                                long absolutes_len = 0;
                                                t_atom *absolutes_atoms = NULL;
                                                atomarray_getatoms(absolutes_atomarray, &absolutes_len, &absolutes_atoms);
                                                for (long k = 0; k < absolutes_len; k++) {
                                                    double old_absolute = atom_getfloat(absolutes_atoms + k);
                                                    atom_setfloat(absolutes_atoms + k, old_absolute + old_song_reach);
                                                }
                                            }
                                        }

                                        // Adjust span in the copied bar
                                        t_atomarray *span_atomarray = NULL;
                                        if (dictionary_hasentry(copied_bar_dict, gensym("span"))) {
                                            dictionary_getatomarray(copied_bar_dict, gensym("span"), (t_object **)&span_atomarray);
                                            if (span_atomarray) {
                                                long span_len = 0;
                                                t_atom *span_atoms = NULL;
                                                atomarray_getatoms(span_atomarray, &span_len, &span_atoms);
                                                for (long k = 0; k < span_len; k++) {
                                                    long old_span_val = atom_getlong(span_atoms + k);
                                                    atom_setlong(span_atoms + k, old_span_val + old_song_reach);
                                                }
                                            }
                                        }

                                        dictionary_appenddictionary(other_track_dict, target_ts_sym, (t_object *)copied_bar_dict);
                                        crucible_verbose_log(x, "Duplicated bar %s from track %s to %s",
                                                             source_ts_sym->s_name, track_keys[i]->s_name, target_ts_sym->s_name);
                                        crucible_output_bar_data(x, copied_bar_dict, t, track_keys[i], other_track_dict);
                                    }
                                }
                            }
                        }
                    }
                }
                if (track_keys) {
                    sysmem_freeptr(track_keys);
                }
            }
        }
    } else {
        crucible_verbose_log(x, "Challenger span for track %s lost.", track_sym->s_name);
    }

    // Cleanup challenger dict for this track
    if (dictionary_hasentry(x->challenger_dict, track_sym)) {
        dictionary_deleteentry(x->challenger_dict, track_sym);
        crucible_verbose_log(x, "Cleaned up challenger data for track %s.", track_sym->s_name);
    }

    object_release((t_object *)incumbent_dict);
}

long crucible_get_bar_length(t_crucible *x) {
    if (x->local_bar_length > 0) {
        return (long)x->local_bar_length;
    }

    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        object_error((t_object *)x, "bar buffer~ not found");
        return 0;
    }

    long bar_length = 0;
    float *samples = buffer_locksamples(b);
    if (samples) {
        if (buffer_getframecount(b) > 0) {
            bar_length = (long)samples[0];
        }
        buffer_unlocksamples(b);
    }

    if (bar_length > 0) {
        x->local_bar_length = (double)bar_length;
        crucible_verbose_log(x, "thread %ld: bar_length changed to %ld", x->instance_id, bar_length);
    }

    return bar_length;
}

void crucible_local_bar_length(t_crucible *x, double f) {
    if (f <= 0) {
        x->local_bar_length = 0;
    } else {
        x->local_bar_length = f;
    }
    crucible_verbose_log(x, "thread %ld: bar_length changed to %ld", x->instance_id, (long)x->local_bar_length);
    crucible_verbose_log(x, "Local bar length set to: %.2f", f);
}

t_dictionary *dictionary_deep_copy(t_dictionary *src) {
   if (!src) return NULL;

   t_dictionary *dest = dictionary_new();
   t_symbol **keys = NULL;
   long numkeys = 0;

   dictionary_getkeys(src, &numkeys, &keys);

   for (long i = 0; i < numkeys; i++) {
       t_symbol *key = keys[i];
       t_atom value;

       dictionary_getatom(src, key, &value);

       if (atom_gettype(&value) == A_OBJ) {
           t_object *obj = atom_getobj(&value);
           if (object_classname_compare(obj, gensym("dictionary"))) {
               t_dictionary *nested_src = (t_dictionary *)obj;
               t_dictionary *nested_dest = dictionary_deep_copy(nested_src);
               dictionary_appenddictionary(dest, key, (t_object *)nested_dest);
           } else if (object_classname_compare(obj, gensym("atomarray"))) {
               t_atomarray *aa_src = (t_atomarray *)obj;
               long aa_len = 0;
               t_atom *aa_atoms = NULL;
               atomarray_getatoms(aa_src, &aa_len, &aa_atoms);
               t_atomarray *aa_dest = atomarray_new(aa_len, aa_atoms);
               dictionary_appendatomarray(dest, key, (t_object *)aa_dest);
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
    if (x->verbose) {
        char *val_str = crucible_atoms_to_string(argc, argv);
        crucible_verbose_log(x, "Received message: %s %s", s->s_name, val_str ? val_str : "");
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
            long num_bars = argc;
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
        crucible_verbose_log(x, "Unparsable message selector: %s", s->s_name);
    }
}


void crucible_assist(t_crucible *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(anything) Message Stream from buildspans, (symbol) Incumbent Dictionary Name"); break;
            case 1: sprintf(s, "(float) Local Bar Length"); break;
        }
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Palette (symbol)"); break;
            case 1: sprintf(s, "Track (int)"); break;
            case 2: sprintf(s, "Bar (int)"); break;
            case 3: sprintf(s, "Offset (float)"); break;
            case 4: sprintf(s, "Reach List: - <track> <reach> <offset>"); break;
            case 5: sprintf(s, "Fill (symbol)"); break;
            case 6: sprintf(s, "Verbose Logging Outlet"); break;
        }
    }
}

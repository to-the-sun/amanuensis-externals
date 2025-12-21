#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_proto.h"
#include "../shared/visualize.h"
#include <math.h>
#include <stdlib.h> // For qsort
#include <string.h> // For isdigit

// Comparison function for qsort
int compare_longs(const void *a, const void *b) {
    long la = *(const long *)a;
    long lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

typedef struct _assemblespans {
    t_object s_obj;
    t_dictionary *working_memory;
    long current_track;
    double current_offset;
    long bar_length;
    t_symbol *current_palette;
    void *span_outlet;
    void *track_outlet;
} t_assemblespans;

// Function prototypes
void *assemblespans_new(void);
void assemblespans_free(t_assemblespans *x);
void assemblespans_clear(t_assemblespans *x);
void assemblespans_list(t_assemblespans *x, t_symbol *s, long argc, t_atom *argv);
void assemblespans_int(t_assemblespans *x, long n);
void assemblespans_offset(t_assemblespans *x, double f);
void assemblespans_bar_length(t_assemblespans *x, long n);
void assemblespans_track(t_assemblespans *x, long n);
void assemblespans_anything(t_assemblespans *x, t_symbol *s, long argc, t_atom *argv);
void assemblespans_assist(t_assemblespans *x, void *b, long m, long a, char *s);
void assemblespans_bang(t_assemblespans *x);
void assemblespans_flush(t_assemblespans *x);
void assemblespans_end_track_span(t_assemblespans *x, t_symbol *track_sym);
void assemblespans_prune_span(t_assemblespans *x, t_symbol *track_sym, long bar_to_keep);
void assemblespans_visualize_memory(t_assemblespans *x);


t_class *assemblespans_class;

void assemblespans_visualize_memory(t_assemblespans *x) {
    long buffer_size = 4096;
    char *json_buffer = (char *)sysmem_newptr(buffer_size);
    if (!json_buffer) {
        object_error((t_object *)x, "Failed to allocate memory for visualization buffer.");
        return;
    }
    long offset = 0;

    offset += snprintf(json_buffer + offset, buffer_size - offset, "{\"working_memory\":{");

    long num_tracks;
    t_symbol **track_keys;
    dictionary_getkeys(x->working_memory, &num_tracks, &track_keys);

    int first_track = 1;
    if (track_keys) {
        for (long i = 0; i < num_tracks; i++) {
            if (buffer_size - offset < 1024) {
                buffer_size *= 2;
                json_buffer = (char *)sysmem_resizeptr(json_buffer, buffer_size);
            }

            if (!first_track) {
                offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
            }
            first_track = 0;

            t_symbol *track_sym = track_keys[i];
            offset += snprintf(json_buffer + offset, buffer_size - offset, "\"%s\":{\"absolutes\":[", track_sym->s_name);

            t_atom track_dict_atom;
            dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
            t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

            long num_bars;
            t_symbol **bar_keys;
            dictionary_getkeys(track_dict, &num_bars, &bar_keys);

            int first_absolute = 1;
            if (bar_keys) {
                for (long j = 0; j < num_bars; j++) {
                    t_symbol *bar_sym = bar_keys[j];
                    char *endptr;
                    strtol(bar_sym->s_name, &endptr, 10);
                    if (*endptr != '\0') continue;

                    t_atom bar_dict_atom;
                    dictionary_getatom(track_dict, bar_sym, &bar_dict_atom);
                    t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);

                    if (dictionary_hasentry(bar_dict, gensym("absolutes"))) {
                        t_atom absolutes_atom;
                        dictionary_getatom(bar_dict, gensym("absolutes"), &absolutes_atom);
                        t_atomarray *absolutes_array = (t_atomarray *)atom_getobj(&absolutes_atom);
                        long absolutes_count = atomarray_getsize(absolutes_array);

                        for (long k = 0; k < absolutes_count; k++) {
                           if (buffer_size - offset < 256) {
                                buffer_size *= 2;
                                json_buffer = (char *)sysmem_resizeptr(json_buffer, buffer_size);
                            }
                            if (!first_absolute) {
                                offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                            }
                            first_absolute = 0;
                            t_atom val_atom;
                            atomarray_getindex(absolutes_array, k, &val_atom);
                            double val = atom_getfloat(&val_atom);
                            offset += snprintf(json_buffer + offset, buffer_size - offset, "%.2f", val);
                        }
                    }
                }
                sysmem_freeptr(bar_keys);
            }
            offset += snprintf(json_buffer + offset, buffer_size - offset, "],\"offsets\":[");

            int first_offset = 1;
            dictionary_getkeys(track_dict, &num_bars, &bar_keys); // Re-fetch keys
            if (bar_keys) {
                 for (long j = 0; j < num_bars; j++) {
                    t_symbol *bar_sym = bar_keys[j];
                    char *endptr;
                    strtol(bar_sym->s_name, &endptr, 10);
                    if (*endptr != '\0') continue;

                    t_atom bar_dict_atom;
                    dictionary_getatom(track_dict, bar_sym, &bar_dict_atom);
                    t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);

                    if (dictionary_hasentry(bar_dict, gensym("offset"))) {
                        if (buffer_size - offset < 256) {
                            buffer_size *= 2;
                            json_buffer = (char *)sysmem_resizeptr(json_buffer, buffer_size);
                        }
                        if (!first_offset) {
                            offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                        }
                        first_offset = 0;
                        t_atom offset_val_atom;
                        dictionary_getatom(bar_dict, gensym("offset"), &offset_val_atom);
                        double val = atom_getfloat(&offset_val_atom);
                        offset += snprintf(json_buffer + offset, buffer_size - offset, "%.2f", val);
                    }
                }
                sysmem_freeptr(bar_keys);
            }
            offset += snprintf(json_buffer + offset, buffer_size - offset, "]}");
        }
        sysmem_freeptr(track_keys);
    }
    offset += snprintf(json_buffer + offset, buffer_size - offset, "},\"current_offset\":%.2f}", x->current_offset);

    visualize(json_buffer);
    sysmem_freeptr(json_buffer);
}


void ext_main(void *r) {
    t_class *c;
    c = class_new("assemblespans", (method)assemblespans_new, (method)assemblespans_free, (short)sizeof(t_assemblespans), 0L, A_GIMME, 0);
    class_addmethod(c, (method)assemblespans_clear, "clear", 0);
    class_addmethod(c, (method)assemblespans_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)assemblespans_int, "int", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_track, "in2", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_bar_length, "in3", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)assemblespans_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)assemblespans_bang, "bang", 0);
    class_register(CLASS_BOX, c);
    assemblespans_class = c;
}

void *assemblespans_new(void) {
    t_assemblespans *x = (t_assemblespans *)object_alloc(assemblespans_class);
    if (x) {
        x->working_memory = dictionary_new();
        x->current_track = 0;
        x->current_offset = 0.0;
        x->bar_length = 125; // Default bar length
        x->current_palette = gensym("");
        // Inlets are created from right to left.
        proxy_new((t_object *)x, 4, NULL); // Palette
        intin((t_object *)x, 3);    // Bar Length
        intin((t_object *)x, 2);    // Track Number
        floatin((t_object *)x, 1);  // Offset

        // Outlets are created from left to right
        x->span_outlet = listout((t_object *)x);
        x->track_outlet = intout((t_object *)x);

        if (visualize_init() != 0) {
            object_error((t_object *)x, "Failed to initialize visualization.");
        }
    }
    return (x);
}

void assemblespans_free(t_assemblespans *x) {
    visualize_cleanup();
    if (x->working_memory) {
        object_free(x->working_memory);
    }
}

void assemblespans_clear(t_assemblespans *x) {
    if (x->working_memory) {
        object_free(x->working_memory);
    }
    x->working_memory = dictionary_new();
    x->current_track = 0;
    x->current_offset = 0.0;
    x->bar_length = 125; // Default bar length
    x->current_palette = gensym("");
    post("assemblespans cleared.");
    assemblespans_visualize_memory(x);
}

// Handler for float messages on the 2nd inlet (proxy #1, offset)
void assemblespans_offset(t_assemblespans *x, double f) {
    x->current_offset = f;
    post("Global offset updated to: %.2f", f);
    assemblespans_visualize_memory(x);
}

// Handler for int messages on the 4th inlet (proxy #3, bar length)
void assemblespans_bar_length(t_assemblespans *x, long n) {
    x->bar_length = n;
    post("Bar length updated to: %ld", n);
}

// Handler for int messages on the 3rd inlet (proxy #2, track number)
void assemblespans_track(t_assemblespans *x, long n) {
    x->current_track = n;
    post("Track updated to: %ld", n);
}

// Handler for various messages, including palette symbol
void assemblespans_anything(t_assemblespans *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet_num = proxy_getinlet((t_object *)x);

    // Inlet 4 is the palette symbol inlet
    if (inlet_num == 4) {
        // A standalone symbol is a message with argc=0
        if (argc == 0) {
            x->current_palette = s;
            post("Palette set to: %s", s->s_name);
        } else {
            // If it has arguments, it's a list starting with a symbol, which we don't handle here.
            object_error((t_object *)x, "Palette inlet expects a single symbol, but received a list.");
        }
    } else {
        // Post an error for unhandled messages on other inlets to avoid silent failures.
        object_error((t_object *)x, "Message '%s' not understood in inlet %ld.", s->s_name, inlet_num);
    }
}


// Handler for list messages on the main inlet
void assemblespans_list(t_assemblespans *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc != 2 || atom_gettype(argv) != A_FLOAT || atom_gettype(argv + 1) != A_FLOAT) {
        object_error((t_object *)x, "Input must be a list of two floats: timestamp and score.");
        return;
    }

    double timestamp = atom_getfloat(argv);
    double score = atom_getfloat(argv + 1);

    post("--- New Timestamp-Score Pair Received ---");
    post("Absolute timestamp: %.2f, Score: %.2f", timestamp, score);

    // Get current track symbol
    char track_str[32];
    snprintf(track_str, 32, "%ld", x->current_track);
    t_symbol *track_sym = gensym(track_str);

    // Get track dictionary (level 1)
    t_dictionary *track_dict;
    if (!dictionary_hasentry(x->working_memory, track_sym)) {
        track_dict = dictionary_new();
        dictionary_appenddictionary(x->working_memory, track_sym, (t_object *)track_dict);
    } else {
        t_atom track_dict_atom;
        dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
        track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);
    }

    // Get current offset
    double offset_val = x->current_offset;
    post("Current offset is: %.2f", offset_val);

    // Subtract offset and calculate bar timestamp
    double relative_timestamp = timestamp - offset_val;
    post("Relative timestamp (absolutes - offset): %.2f", relative_timestamp);

    long bar_timestamp_val = floor(relative_timestamp / x->bar_length) * x->bar_length;
    post("Calculated bar timestamp (rounded down to nearest %ld): %ld", x->bar_length, bar_timestamp_val);

    // --- Find the most recent bar to see if this is a new bar ---
    long last_bar_timestamp = -1;
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(track_dict, &num_keys, &keys);
    if (keys && num_keys > 0) {
        for (long i = 0; i < num_keys; i++) {
            char *key_str = keys[i]->s_name;
            char *endptr;
            long val = strtol(key_str, &endptr, 10);
            if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) { // It's a numeric key, not just "-"
                if (last_bar_timestamp == -1 || val > last_bar_timestamp) {
                    last_bar_timestamp = val;
                }
            }
        }
        sysmem_freeptr(keys);
    }

    int is_new_bar = (last_bar_timestamp == -1 || bar_timestamp_val > last_bar_timestamp);

    // --- Deferred span ending logic (only if a new bar is detected) ---
    if (is_new_bar && last_bar_timestamp != -1) {
        // 1. Check for discontiguity first.
        if (bar_timestamp_val > last_bar_timestamp + x->bar_length) {
            post("Discontiguous bar detected. New bar %ld is more than %ldms after last bar %ld.",
                 bar_timestamp_val, x->bar_length, last_bar_timestamp);
            assemblespans_end_track_span(x, track_sym);
            if (!dictionary_hasentry(x->working_memory, track_sym)) {
                track_dict = dictionary_new();
                dictionary_appenddictionary(x->working_memory, track_sym, (t_object *)track_dict);
            } else {
                t_atom track_dict_atom;
                dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
                track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);
            }
        } else {
            // 2. Deferred rating check on the *previous* bar.
            long num_span_keys;
            t_symbol **span_keys;
            dictionary_getkeys(track_dict, &num_span_keys, &span_keys);

            if (span_keys && num_span_keys > 1) { // Need at least two bars to compare.
                // RATING WITH LAST BAR
                double lowest_mean_with = -1.0;
                for (long i = 0; i < num_span_keys; i++) {
                    char *key_str = span_keys[i]->s_name;
                    char *endptr;
                    strtol(key_str, &endptr, 10);
                    if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) {
                         t_atom bar_dict_atom;
                        dictionary_getatom(track_dict, span_keys[i], &bar_dict_atom);
                        t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                        if(dictionary_hasentry(bar_dict, gensym("mean"))) {
                            t_atom mean_atom;
                            dictionary_getatom(bar_dict, gensym("mean"), &mean_atom);
                            double bar_mean = atom_getfloat(&mean_atom);
                             if (lowest_mean_with == -1.0 || bar_mean < lowest_mean_with) {
                                lowest_mean_with = bar_mean;
                            }
                        }
                    }
                }
                double rating_with = lowest_mean_with * num_span_keys;

                // RATING WITHOUT LAST BAR
                double lowest_mean_without = -1.0;
                long bars_without_count = 0;
                char last_bar_str[32];
                snprintf(last_bar_str, 32, "%ld", last_bar_timestamp);
                t_symbol *last_bar_sym = gensym(last_bar_str);

                for (long i = 0; i < num_span_keys; i++) {
                    if (span_keys[i] != last_bar_sym) {
                        char *key_str = span_keys[i]->s_name;
                        char *endptr;
                        strtol(key_str, &endptr, 10);
                        if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) {
                             t_atom bar_dict_atom;
                            dictionary_getatom(track_dict, span_keys[i], &bar_dict_atom);
                            t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                            if(dictionary_hasentry(bar_dict, gensym("mean"))) {
                                t_atom mean_atom;
                                dictionary_getatom(bar_dict, gensym("mean"), &mean_atom);
                                double bar_mean = atom_getfloat(&mean_atom);
                                if (lowest_mean_without == -1.0 || bar_mean < lowest_mean_without) {
                                    lowest_mean_without = bar_mean;
                                }
                            }
                             bars_without_count++;
                        }
                    }
                }
                double rating_without = (bars_without_count > 0) ? (lowest_mean_without * bars_without_count) : 0.0;

                if (rating_with < rating_without) {
                    post("Deferred rating check: Including bar %ld decreased rating (%.2f -> %.2f). Pruning span.",
                         last_bar_timestamp, rating_without, rating_with);
                    assemblespans_prune_span(x, track_sym, last_bar_timestamp);
                } else {
                    post("Deferred rating check: Including bar %ld did not decrease rating (%.2f -> %.2f). Continuing span.",
                         last_bar_timestamp, rating_without, rating_with);
                }
            }
            if (span_keys) sysmem_freeptr(span_keys);
        }
    }


    // --- ADD OR UPDATE BAR ---
    char bar_str[32];
    snprintf(bar_str, 32, "%ld", bar_timestamp_val);
    t_symbol *bar_sym = gensym(bar_str);

    t_dictionary *bar_dict;
    if (!dictionary_hasentry(track_dict, bar_sym)) {
        bar_dict = dictionary_new();
        dictionary_appenddictionary(track_dict, bar_sym, (t_object *)bar_dict);
        post("Created new dictionary for: %s::%s", track_sym->s_name, bar_sym->s_name);
    } else {
        t_atom bar_dict_atom;
        dictionary_getatom(track_dict, bar_sym, &bar_dict_atom);
        bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
    }

    if (dictionary_hasentry(bar_dict, gensym("offset"))) dictionary_deleteentry(bar_dict, gensym("offset"));
    dictionary_appendfloat(bar_dict, gensym("offset"), x->current_offset);
    post("Updated '%s' for bar %s to %.2f", "offset", bar_sym->s_name, x->current_offset);

    if (dictionary_hasentry(bar_dict, gensym("palette"))) dictionary_deleteentry(bar_dict, gensym("palette"));
    dictionary_appendsym(bar_dict, gensym("palette"), x->current_palette);
    post("Updated '%s' for bar %s to %s", "palette", bar_sym->s_name, x->current_palette->s_name);

    t_atomarray *absolutes_array;
    if (!dictionary_hasentry(bar_dict, gensym("absolutes"))) {
        absolutes_array = atomarray_new(0, NULL);
        t_atom a; atom_setobj(&a, (t_object *)absolutes_array);
        dictionary_appendatom(bar_dict, gensym("absolutes"), &a);
    } else {
        t_atom a; dictionary_getatom(bar_dict, gensym("absolutes"), &a);
        absolutes_array = (t_atomarray *)atom_getobj(&a);
    }
    t_atom new_absolute; atom_setfloat(&new_absolute, timestamp);
    atomarray_appendatom(absolutes_array, &new_absolute);

    t_atomarray *scores_array;
    if (!dictionary_hasentry(bar_dict, gensym("scores"))) {
        scores_array = atomarray_new(0, NULL);
        t_atom a; atom_setobj(&a, (t_object *)scores_array);
        dictionary_appendatom(bar_dict, gensym("scores"), &a);
    } else {
        t_atom a; dictionary_getatom(bar_dict, gensym("scores"), &a);
        scores_array = (t_atomarray *)atom_getobj(&a);
    }
    t_atom new_score; atom_setfloat(&new_score, score);
    atomarray_appendatom(scores_array, &new_score);

    long scores_count = atomarray_getsize(scores_array);
    if (scores_count > 0) {
        double sum = 0.0;
        long count; t_atom *atoms;
        atomarray_getatoms(scores_array, &count, &atoms);
        for (long i = 0; i < count; i++) sum += atom_getfloat(&atoms[i]);
        double mean = sum / count;
        if (dictionary_hasentry(bar_dict, gensym("mean"))) dictionary_deleteentry(bar_dict, gensym("mean"));
        dictionary_appendfloat(bar_dict, gensym("mean"), mean);
        post("Updated '%s' for bar %s to %.2f", "mean", bar_sym->s_name, mean);
    }

    // --- UPDATE AND BACK-PROPAGATE SPAN ---
    long num_all_keys;
    t_symbol **all_keys;
    dictionary_getkeys(track_dict, &num_all_keys, &all_keys);
    long bar_timestamps_count = 0;
    long *bar_timestamps = (long *)sysmem_newptr(num_all_keys * sizeof(long));
    if (all_keys) {
        for (long i = 0; i < num_all_keys; i++) {
            char *key_str = all_keys[i]->s_name;
            char *endptr;
            long val = strtol(key_str, &endptr, 10);
            if (*endptr == '\0') bar_timestamps[bar_timestamps_count++] = val;
        }
        sysmem_freeptr(all_keys);
    }
    qsort(bar_timestamps, bar_timestamps_count, sizeof(long), compare_longs);
    t_atomarray *span_array = atomarray_new(0, NULL);
    for (long i = 0; i < bar_timestamps_count; i++) {
        t_atom a;
        atom_setlong(&a, bar_timestamps[i]);
        atomarray_appendatom(span_array, &a);
    }
    for (long i = 0; i < bar_timestamps_count; i++) {
        char temp_bar_str[32];
        snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
        t_symbol *temp_bar_sym = gensym(temp_bar_str);
        t_atom bar_dict_atom;
        dictionary_getatom(track_dict, temp_bar_sym, &bar_dict_atom);
        t_dictionary *temp_bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
        t_atom span_atom;
        atom_setobj(&span_atom, (t_object *)span_array);
        if (dictionary_hasentry(temp_bar_dict, gensym("span"))) dictionary_deleteentry(temp_bar_dict, gensym("span"));
        dictionary_appendatom(temp_bar_dict, gensym("span"), &span_atom);
    }

    // --- RATING CALCULATION & BACK-PROPAGATION ---
    double final_lowest_mean = -1.0;
    for (long i = 0; i < bar_timestamps_count; i++) {
        char temp_bar_str[32];
        snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
        t_symbol *temp_bar_sym = gensym(temp_bar_str);
        t_atom bar_dict_atom;
        dictionary_getatom(track_dict, temp_bar_sym, &bar_dict_atom);
        t_dictionary *temp_bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
        if (dictionary_hasentry(temp_bar_dict, gensym("mean"))) {
            t_atom mean_atom;
            dictionary_getatom(temp_bar_dict, gensym("mean"), &mean_atom);
            double current_bar_mean = atom_getfloat(&mean_atom);
            if (final_lowest_mean == -1.0 || current_bar_mean < final_lowest_mean) {
                final_lowest_mean = current_bar_mean;
            }
        }
    }
    if (final_lowest_mean != -1.0) {
        double final_rating = final_lowest_mean * bar_timestamps_count;
        for (long i = 0; i < bar_timestamps_count; i++) {
            char temp_bar_str[32];
            snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);
            t_atom bar_dict_atom;
            dictionary_getatom(track_dict, temp_bar_sym, &bar_dict_atom);
            t_dictionary *temp_bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
            if (dictionary_hasentry(temp_bar_dict, gensym("rating"))) dictionary_deleteentry(temp_bar_dict, gensym("rating"));
            dictionary_appendfloat(temp_bar_dict, gensym("rating"), final_rating);
        }
        post("Final rating for span: %.2f", final_rating);
    }

    sysmem_freeptr(bar_timestamps);
    assemblespans_visualize_memory(x);
}

void assemblespans_end_track_span(t_assemblespans *x, t_symbol *track_sym) {
    if (!dictionary_hasentry(x->working_memory, track_sym)) {
        return;
    }

    t_atom track_dict_atom;
    dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
    t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

    t_atomarray *span_to_output = NULL;
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(track_dict, &num_keys, &keys);

    if (keys) {
        // Find the first valid span in the track. Any bar will have it.
        for (long j = 0; j < num_keys; j++) {
            char *key_str = keys[j]->s_name;
            char *endptr;
            strtol(key_str, &endptr, 10);
            if (*endptr != '\0') continue; // Skip non-numeric keys

            t_atom bar_dict_atom;
            dictionary_getatom(track_dict, keys[j], &bar_dict_atom);
            if (atom_gettype(&bar_dict_atom) == A_OBJ && object_classname(atom_getobj(&bar_dict_atom)) == gensym("dictionary")) {
                t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                if (dictionary_hasentry(bar_dict, gensym("span"))) {
                    t_atom span_atom;
                    dictionary_getatom(bar_dict, gensym("span"), &span_atom);
                    span_to_output = (t_atomarray *)atom_getobj(&span_atom);
                    break;
                }
            }
        }
        sysmem_freeptr(keys);
    }

    if (span_to_output) {
        post("Ending span for track %s...", track_sym->s_name);
        outlet_int(x->track_outlet, atol(track_sym->s_name));
        t_atom *atoms = NULL;
        long atom_count = 0;
        atomarray_getatoms(span_to_output, &atom_count, &atoms);
        outlet_list(x->span_outlet, NULL, atom_count, atoms);
    }

    // Free the entire track dictionary object. This recursively frees all sub-dictionaries and atomarrays.
    object_free((t_object *)track_dict);

    // Remove the entry for the track from the main working_memory.
    dictionary_deleteentry(x->working_memory, track_sym);
    assemblespans_visualize_memory(x);
}

void assemblespans_bang(t_assemblespans *x) {
    post("Flush triggered by bang.");
    assemblespans_flush(x);
}

void assemblespans_flush(t_assemblespans *x) {
    long num_tracks;
    t_symbol **tracks;
    dictionary_getkeys(x->working_memory, &num_tracks, &tracks);
    if (!tracks) return;

    for (long i = 0; i < num_tracks; i++) {
        assemblespans_end_track_span(x, tracks[i]);
    }
    if (tracks) sysmem_freeptr(tracks);
}


// Handler for int messages on the main inlet
void assemblespans_int(t_assemblespans *x, long n) {
    object_error((t_object *)x, "Invalid input: main inlet expects a list of two floats.");
}

void assemblespans_assist(t_assemblespans *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0:
                sprintf(s, "(list) Timestamp-Score Pair, (bang) Flush, (clear) Clear");
                break;
            case 1:
                sprintf(s, "(float) Offset Timestamp");
                break;
            case 2:
                sprintf(s, "(int) Track Number");
                break;
            case 3:
                sprintf(s, "(int) Bar Length");
                break;
            case 4:
                sprintf(s, "(symbol) Palette");
                break;
        }
    }
}

void assemblespans_prune_span(t_assemblespans *x, t_symbol *track_sym, long bar_to_keep) {
    if (!dictionary_hasentry(x->working_memory, track_sym)) {
        return;
    }

    t_atom track_dict_atom;
    dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
    t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(track_dict, &num_keys, &keys);
    if (!keys) return;

    // Create a sorted list of bars to flush
    long *bars_to_flush = (long *)sysmem_newptr(num_keys * sizeof(long));
    long flush_count = 0;
    for (long i = 0; i < num_keys; i++) {
        char *key_str = keys[i]->s_name;
        char *endptr;
        long val = strtol(key_str, &endptr, 10);
         if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1) && val != bar_to_keep) {
            bars_to_flush[flush_count++] = val;
        }
    }
    qsort(bars_to_flush, flush_count, sizeof(long), compare_longs);

    // Create an atomarray from the sorted list for output
    t_atomarray *span_to_output = atomarray_new(0, NULL);
    for(long i = 0; i < flush_count; i++) {
        t_atom a;
        atom_setlong(&a, bars_to_flush[i]);
        atomarray_appendatom(span_to_output, &a);
    }

    // Output the flushed span
    if (atomarray_getsize(span_to_output) > 0) {
        post("Pruning span for track %s, keeping bar %ld", track_sym->s_name, bar_to_keep);
        outlet_int(x->track_outlet, atol(track_sym->s_name));
        t_atom *atoms = NULL;
        long atom_count = 0;
        atomarray_getatoms(span_to_output, &atom_count, &atoms);
        outlet_list(x->span_outlet, NULL, atom_count, atoms);
    }
    object_free(span_to_output); // Free the temporary atomarray

    // Remove the flushed bars from the track dictionary
    for(long i = 0; i < flush_count; i++) {
        char bar_str[32];
        snprintf(bar_str, 32, "%ld", bars_to_flush[i]);
        t_symbol *bar_sym_to_delete = gensym(bar_str);

        if (dictionary_hasentry(track_dict, bar_sym_to_delete)) {
            t_atom bar_dict_atom;
            dictionary_getatom(track_dict, bar_sym_to_delete, &bar_dict_atom);
            object_free(atom_getobj(&bar_dict_atom)); // This frees the sub-dictionary and its contents
            dictionary_deleteentry(track_dict, bar_sym_to_delete);
        }
    }

    sysmem_freeptr(bars_to_flush);
    sysmem_freeptr(keys);

    assemblespans_visualize_memory(x);
}

#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_proto.h"
#include "../shared/visualize.h"
#include <math.h>
#include <stdlib.h> // For qsort
#include <string.h> // For isdigit

// Helper function to convert an atomarray to a string for posting
char* atomarray_to_string(t_atomarray *arr) {
    if (!arr) return NULL;

    long count = atomarray_getsize(arr);
    if (count == 0) {
        char *empty_str = (char *)sysmem_newptr(3);
        strcpy(empty_str, "[]");
        return empty_str;
    }

    long buffer_size = 256; // Initial size
    char *buffer = (char *)sysmem_newptr(buffer_size);
    long offset = 0;

    offset += snprintf(buffer + offset, buffer_size - offset, "[");

    for (long i = 0; i < count; i++) {
        t_atom a;
        atomarray_getindex(arr, i, &a);

        char temp_str[64];
        if (atom_gettype(&a) == A_FLOAT) {
            snprintf(temp_str, 64, "%.2f", atom_getfloat(&a));
        } else if (atom_gettype(&a) == A_LONG) {
            snprintf(temp_str, 64, "%ld", atom_getlong(&a));
        } else {
            snprintf(temp_str, 64, "?");
        }

        if (buffer_size - offset < strlen(temp_str) + 4) { // +4 for ", ]\0"
            buffer_size *= 2;
            buffer = (char *)sysmem_resizeptr(buffer, buffer_size);
            if (!buffer) return NULL;
        }

        offset += snprintf(buffer + offset, buffer_size - offset, "%s", temp_str);
        if (i < count - 1) {
            offset += snprintf(buffer + offset, buffer_size - offset, ", ");
        }
    }

    snprintf(buffer + offset, buffer_size - offset, "]");
    return buffer;
}


// Comparison function for qsort
int compare_longs(const void *a, const void *b) {
    long la = *(const long *)a;
    long lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

typedef struct _buildspans {
    t_object s_obj;
    t_dictionary *building;
    long current_track;
    double current_offset;
    long bar_length;
    t_symbol *current_palette;
    void *span_outlet;
    void *track_outlet;
    void *log_outlet;
} t_buildspans;

// Function prototypes
void *buildspans_new(void);
void buildspans_free(t_buildspans *x);
void buildspans_clear(t_buildspans *x);
void buildspans_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_int(t_buildspans *x, long n);
void buildspans_offset(t_buildspans *x, double f);
void buildspans_bar_length(t_buildspans *x, long n);
void buildspans_track(t_buildspans *x, long n);
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_assist(t_buildspans *x, void *b, long m, long a, char *s);
void buildspans_bang(t_buildspans *x);
void buildspans_flush(t_buildspans *x);
void buildspans_end_track_span(t_buildspans *x, t_symbol *track_sym);
void buildspans_prune_span(t_buildspans *x, t_symbol *track_sym, long bar_to_keep);
void buildspans_visualize_memory(t_buildspans *x);
void buildspans_log_update(t_buildspans *x, t_symbol *track, t_symbol *bar, t_symbol *key, long argc, t_atom *argv);
void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *track_sym, t_symbol *bar_sym);


// Helper function to send log messages out the log outlet
void buildspans_log_update(t_buildspans *x, t_symbol *track, t_symbol *bar, t_symbol *key, long argc, t_atom *argv) {
    char log_key_str[256];
    snprintf(log_key_str, 256, "%s::%s::%s", track->s_name, bar->s_name, key->s_name);
    outlet_anything(x->log_outlet, gensym(log_key_str), argc, argv);
}


t_class *buildspans_class;

void buildspans_visualize_memory(t_buildspans *x) {
    long buffer_size = 4096;
    char *json_buffer = (char *)sysmem_newptr(buffer_size);
    if (!json_buffer) {
        object_error((t_object *)x, "Failed to allocate memory for visualization buffer.");
        return;
    }
    long offset = 0;

    offset += snprintf(json_buffer + offset, buffer_size - offset, "{\"building\":{");

    long num_tracks;
    t_symbol **track_keys;
    dictionary_getkeys(x->building, &num_tracks, &track_keys);

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
            dictionary_getatom(x->building, track_sym, &track_dict_atom);
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
    c = class_new("buildspans", (method)buildspans_new, (method)buildspans_free, (short)sizeof(t_buildspans), 0L, A_GIMME, 0);
    class_addmethod(c, (method)buildspans_clear, "clear", 0);
    class_addmethod(c, (method)buildspans_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_int, "int", A_LONG, 0);
    class_addmethod(c, (method)buildspans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_track, "in2", A_LONG, 0);
    class_addmethod(c, (method)buildspans_bar_length, "in3", A_LONG, 0);
    class_addmethod(c, (method)buildspans_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)buildspans_bang, "bang", 0);
    class_register(CLASS_BOX, c);
    buildspans_class = c;
}

void *buildspans_new(void) {
    t_buildspans *x = (t_buildspans *)object_alloc(buildspans_class);
    if (x) {
        x->building = dictionary_new();
        x->current_track = 0;
        x->current_offset = 0.0;
        x->bar_length = 125; // Default bar length
        x->current_palette = gensym("");
        // Inlets are created from right to left.
        proxy_new((t_object *)x, 4, NULL); // Palette
        intin((t_object *)x, 3);    // Bar Length
        intin((t_object *)x, 2);    // Track Number
        floatin((t_object *)x, 1);  // Offset

        // Outlets are created from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL); // Generic outlet for logs
        x->track_outlet = intout((t_object *)x);
        x->span_outlet = listout((t_object *)x);

        if (visualize_init() != 0) {
            object_error((t_object *)x, "Failed to initialize visualization.");
        }
    }
    return (x);
}

void buildspans_free(t_buildspans *x) {
    visualize_cleanup();
    if (x->building) {
        object_free(x->building);
    }
}

void buildspans_clear(t_buildspans *x) {
    if (x->building) {
        object_free(x->building);
    }
    x->building = dictionary_new();
    x->current_track = 0;
    x->current_offset = 0.0;
    x->bar_length = 125; // Default bar length
    x->current_palette = gensym("");
    post("buildspans cleared.");
    buildspans_visualize_memory(x);
}

// Handler for float messages on the 2nd inlet (proxy #1, offset)
void buildspans_offset(t_buildspans *x, double f) {
    x->current_offset = f;
    post("Global offset updated to: %.2f", f);
    buildspans_visualize_memory(x);
}

// Handler for int messages on the 4th inlet (proxy #3, bar length)
void buildspans_bar_length(t_buildspans *x, long n) {
    x->bar_length = n;
    post("Bar length updated to: %ld", n);
}

// Handler for int messages on the 3rd inlet (proxy #2, track number)
void buildspans_track(t_buildspans *x, long n) {
    x->current_track = n;
    post("Track updated to: %ld", n);
}

// Handler for various messages, including palette symbol
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
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
void buildspans_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
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
    if (!dictionary_hasentry(x->building, track_sym)) {
        track_dict = dictionary_new();
        dictionary_appenddictionary(x->building, track_sym, (t_object *)track_dict);
    } else {
        t_atom track_dict_atom;
        dictionary_getatom(x->building, track_sym, &track_dict_atom);
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

        // 1. DEFERRED RATING CHECK (NOW HAPPENS FIRST)
        long num_span_keys;
        t_symbol **span_keys;
        dictionary_getkeys(track_dict, &num_span_keys, &span_keys);

        int prune_span = 0;

        if (span_keys && num_span_keys > 1) {
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
                         if (lowest_mean_with == -1.0 || bar_mean < lowest_mean_with) lowest_mean_with = bar_mean;
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
            double last_bar_mean = 0.0;

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
                        if (span_keys[i] == last_bar_sym) last_bar_mean = bar_mean;
                        if (span_keys[i] != last_bar_sym) {
                            if (lowest_mean_without == -1.0 || bar_mean < lowest_mean_without) lowest_mean_without = bar_mean;
                            bars_without_count++;
                        }
                    }
                }
            }
            double rating_without = (bars_without_count > 0) ? (lowest_mean_without * bars_without_count) : 0.0;

            if (rating_with < rating_without) {
                post("Deferred rating check: Including bar %ld decreased rating (%.2f -> %.2f). Pruning span.", last_bar_timestamp, rating_without, rating_with);
                prune_span = 1;
            } else if (last_bar_mean > rating_with) {
                post("Deferred rating check: Bar %ld's individual rating (%.2f) is higher than span rating if included (%.2f). Pruning span.", last_bar_timestamp, last_bar_mean, rating_with);
                prune_span = 1;
            } else {
                post("Deferred rating check: Including bar %ld did not decrease rating (%.2f -> %.2f) and is not better on its own. Continuing span.", last_bar_timestamp, rating_without, rating_with);
            }

            if (prune_span) buildspans_prune_span(x, track_sym, last_bar_timestamp);
        }
        if (span_keys) sysmem_freeptr(span_keys);

        // 2. DISCONTIGUITY CHECK (NOW HAPPENS SECOND)
        long most_recent_bar_after_rating_check = -1;
        dictionary_getkeys(track_dict, &num_span_keys, &span_keys);
         if (span_keys && num_span_keys > 0) {
            for (long i = 0; i < num_span_keys; i++) {
                char *key_str = span_keys[i]->s_name;
                char *endptr;
                long val = strtol(key_str, &endptr, 10);
                if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) {
                    if (most_recent_bar_after_rating_check == -1 || val > most_recent_bar_after_rating_check) {
                        most_recent_bar_after_rating_check = val;
                    }
                }
            }
            sysmem_freeptr(span_keys);
        }

        if (most_recent_bar_after_rating_check != -1 && bar_timestamp_val > most_recent_bar_after_rating_check + x->bar_length) {
            post("Discontiguous bar detected. New bar %ld is more than %ldms after last bar %ld.", bar_timestamp_val, x->bar_length, most_recent_bar_after_rating_check);
            buildspans_end_track_span(x, track_sym);
            if (!dictionary_hasentry(x->building, track_sym)) {
                track_dict = dictionary_new();
                dictionary_appenddictionary(x->building, track_sym, (t_object *)track_dict);
            } else {
                t_atom track_dict_atom;
                dictionary_getatom(x->building, track_sym, &track_dict_atom);
                track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);
            }
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
    post("%s::%s::%s %.2f", track_sym->s_name, bar_sym->s_name, "offset", x->current_offset);
    t_atom offset_atom;
    atom_setfloat(&offset_atom, x->current_offset);
    buildspans_log_update(x, track_sym, bar_sym, gensym("offset"), 1, &offset_atom);

    if (dictionary_hasentry(bar_dict, gensym("palette"))) dictionary_deleteentry(bar_dict, gensym("palette"));
    dictionary_appendsym(bar_dict, gensym("palette"), x->current_palette);
    post("%s::%s::%s %s", track_sym->s_name, bar_sym->s_name, "palette", x->current_palette->s_name);
    t_atom palette_atom;
    atom_setsym(&palette_atom, x->current_palette);
    buildspans_log_update(x, track_sym, bar_sym, gensym("palette"), 1, &palette_atom);

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
    char *abs_str = atomarray_to_string(absolutes_array);
    if (abs_str) {
        post("%s::%s::%s %s", track_sym->s_name, bar_sym->s_name, "absolutes", abs_str);
        sysmem_freeptr(abs_str);
        long count; t_atom *atoms;
        atomarray_getatoms(absolutes_array, &count, &atoms);
        buildspans_log_update(x, track_sym, bar_sym, gensym("absolutes"), count, atoms);
    }

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
    char *scores_str = atomarray_to_string(scores_array);
    if (scores_str) {
        post("%s::%s::%s %s", track_sym->s_name, bar_sym->s_name, "scores", scores_str);
        sysmem_freeptr(scores_str);
        long count; t_atom *atoms;
        atomarray_getatoms(scores_array, &count, &atoms);
        buildspans_log_update(x, track_sym, bar_sym, gensym("scores"), count, atoms);
    }

    long scores_count = atomarray_getsize(scores_array);
    if (scores_count > 0) {
        double sum = 0.0;
        long count; t_atom *atoms;
        atomarray_getatoms(scores_array, &count, &atoms);
        for (long i = 0; i < count; i++) sum += atom_getfloat(&atoms[i]);
        double mean = sum / count;
        if (dictionary_hasentry(bar_dict, gensym("mean"))) dictionary_deleteentry(bar_dict, gensym("mean"));
        dictionary_appendfloat(bar_dict, gensym("mean"), mean);
        post("%s::%s::%s %.2f", track_sym->s_name, bar_sym->s_name, "mean", mean);
        t_atom mean_atom;
        atom_setfloat(&mean_atom, mean);
        buildspans_log_update(x, track_sym, bar_sym, gensym("mean"), 1, &mean_atom);
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
    }

    qsort(bar_timestamps, bar_timestamps_count, sizeof(long), compare_longs);

    // 1. Get a reference to the OLD shared span array before we do anything else.
    t_atomarray *old_span_array = NULL;
    if (bar_timestamps_count > 1 && all_keys) { // only need to do this if there was a previous span
        for (long i = 0; i < num_all_keys; i++) {
             char *key_str = all_keys[i]->s_name;
            char *endptr;
            strtol(key_str, &endptr, 10);
            if (*endptr == '\0') {
                 t_atom bar_dict_atom;
                dictionary_getatom(track_dict, all_keys[i], &bar_dict_atom);
                t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                if (dictionary_hasentry(bar_dict, gensym("span"))) {
                    t_atom span_atom;
                    dictionary_getatom(bar_dict, gensym("span"), &span_atom);
                    old_span_array = (t_atomarray *)atom_getobj(&span_atom);
                    break; // Found it, we can stop.
                }
            }
        }
    }


    // 2. Create the NEW span array.
    t_atomarray *new_span_array = atomarray_new(0, NULL);
    for (long i = 0; i < bar_timestamps_count; i++) {
        t_atom a;
        atom_setlong(&a, bar_timestamps[i]);
        atomarray_appendatom(new_span_array, &a);
    }
    t_atom new_span_atom;
    atom_setobj(&new_span_atom, (t_object *)new_span_array);

    // 3. Unlink the OLD span array from all bars.
    if (old_span_array) {
        long old_span_size = 0;
        t_atom *old_span_atoms = NULL;
        atomarray_getatoms(old_span_array, &old_span_size, &old_span_atoms);

        for (long i = 0; i < old_span_size; i++) {
            long old_bar_ts = atom_getlong(&old_span_atoms[i]);
            char temp_bar_str[32];
            snprintf(temp_bar_str, 32, "%ld", old_bar_ts);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);
            if (dictionary_hasentry(track_dict, temp_bar_sym)) {
                t_atom bar_dict_atom;
                dictionary_getatom(track_dict, temp_bar_sym, &bar_dict_atom);
                t_dictionary *temp_bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                if (dictionary_hasentry(temp_bar_dict, gensym("span"))) {
                    dictionary_deleteentry(temp_bar_dict, gensym("span"));
                }
            }
        }
    }


    // 4. Link the NEW span array to all bars and log it.
    char *span_str = atomarray_to_string(new_span_array);
    if (span_str) {
        for (long i = 0; i < bar_timestamps_count; i++) {
            char temp_bar_str[32];
            snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);
            if (dictionary_hasentry(track_dict, temp_bar_sym)) {
                t_atom bar_dict_atom;
                dictionary_getatom(track_dict, temp_bar_sym, &bar_dict_atom);
                t_dictionary *temp_bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                dictionary_appendatom(temp_bar_dict, gensym("span"), &new_span_atom);

                // Logging
                post("%s::%s::%s %s", track_sym->s_name, temp_bar_sym->s_name, "span", span_str);
                long count; t_atom *atoms;
                atomarray_getatoms(new_span_array, &count, &atoms);
                buildspans_log_update(x, track_sym, temp_bar_sym, gensym("span"), count, atoms);
            }
        }
        sysmem_freeptr(span_str);
    }

    if (all_keys) sysmem_freeptr(all_keys);


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
            post("%s::%s::%s %.2f", track_sym->s_name, temp_bar_sym->s_name, "rating", final_rating);
            t_atom rating_atom;
            atom_setfloat(&rating_atom, final_rating);
            buildspans_log_update(x, track_sym, temp_bar_sym, gensym("rating"), 1, &rating_atom);
        }
        post("Final rating for span: %.2f (%.2f * %ld)", final_rating, final_lowest_mean, bar_timestamps_count);
    }

    sysmem_freeptr(bar_timestamps);
    buildspans_visualize_memory(x);
}

void buildspans_end_track_span(t_buildspans *x, t_symbol *track_sym) {
    if (!dictionary_hasentry(x->building, track_sym)) return;

    t_atom track_dict_atom;
    dictionary_getatom(x->building, track_sym, &track_dict_atom);
    t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

    t_atomarray *span_to_output = NULL;
    int local_span_created = 0;

    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(track_dict, &num_keys, &keys);

    if (keys) {
        for (long j = 0; j < num_keys; j++) {
            char *key_str = keys[j]->s_name;
            char *endptr;
            strtol(key_str, &endptr, 10);
            if (*endptr != '\0') continue;

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

        if (!span_to_output && num_keys > 0) {
            local_span_created = 1;
            long *bar_timestamps = (long *)sysmem_newptr(num_keys * sizeof(long));
            long bar_count = 0;
            for (long i = 0; i < num_keys; i++) {
                char *key_str = keys[i]->s_name;
                char *endptr;
                long val = strtol(key_str, &endptr, 10);
                if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) {
                    bar_timestamps[bar_count++] = val;
                }
            }
            qsort(bar_timestamps, bar_count, sizeof(long), compare_longs);
            span_to_output = atomarray_new(0, NULL);
            for (long i = 0; i < bar_count; i++) {
                t_atom a;
                atom_setlong(&a, bar_timestamps[i]);
                atomarray_appendatom(span_to_output, &a);
            }
            sysmem_freeptr(bar_timestamps);
        }
        sysmem_freeptr(keys);
    }

    if (span_to_output) {
        long span_size;
        t_atom *span_atoms = NULL;
        atomarray_getatoms(span_to_output, &span_size, &span_atoms);

        double lowest_mean = -1.0;
        if (span_size > 0) {
            for (long i = 0; i < span_size; i++) {
                long bar_ts = atom_getlong(&span_atoms[i]);
                char bar_str[32];
                snprintf(bar_str, 32, "%ld", bar_ts);
                t_symbol *bar_sym = gensym(bar_str);

                if (dictionary_hasentry(track_dict, bar_sym)) {
                    t_atom bar_dict_atom;
                    dictionary_getatom(track_dict, bar_sym, &bar_dict_atom);
                    t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                    if (dictionary_hasentry(bar_dict, gensym("mean"))) {
                        t_atom mean_atom;
                        dictionary_getatom(bar_dict, gensym("mean"), &mean_atom);
                        double bar_mean = atom_getfloat(&mean_atom);
                        if (lowest_mean == -1.0 || bar_mean < lowest_mean) {
                            lowest_mean = bar_mean;
                        }
                    }
                }
            }
        }
        double final_rating = (lowest_mean != -1.0) ? (lowest_mean * span_size) : 0.0;
        post("Ending span for track %s with rating %.2f (%.2f * %ld)", track_sym->s_name, final_rating, lowest_mean, span_size);

        outlet_int(x->track_outlet, atol(track_sym->s_name));
        outlet_list(x->span_outlet, NULL, span_size, span_atoms);

        // Reset all bars in the ended span to standalone state before freeing them.
        for (long i = 0; i < span_size; i++) {
            long bar_ts = atom_getlong(&span_atoms[i]);
            char bar_str[32];
            snprintf(bar_str, 32, "%ld", bar_ts);
            buildspans_reset_bar_to_standalone(x, track_sym, gensym(bar_str));
        }

        if (local_span_created) {
            object_free(span_to_output);
        }
    }

    object_free((t_object *)track_dict);
    dictionary_deleteentry(x->building, track_sym);
    buildspans_visualize_memory(x);
}

void buildspans_bang(t_buildspans *x) {
    post("Flush triggered by bang.");
    buildspans_flush(x);
}

void buildspans_flush(t_buildspans *x) {
    long num_tracks;
    t_symbol **tracks;
    dictionary_getkeys(x->building, &num_tracks, &tracks);
    if (!tracks) return;

    for (long i = 0; i < num_tracks; i++) {
        buildspans_end_track_span(x, tracks[i]);
    }
    if (tracks) sysmem_freeptr(tracks);
}


// Handler for int messages on the main inlet
void buildspans_int(t_buildspans *x, long n) {
    object_error((t_object *)x, "Invalid input: main inlet expects a list of two floats.");
}

void buildspans_assist(t_buildspans *x, void *b, long m, long a, char *s) {
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
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Span Data (list)"); break;
            case 1: sprintf(s, "Track Number (int)"); break;
            case 2: sprintf(s, "Log Messages (anything)"); break;
        }
    }
}

void buildspans_prune_span(t_buildspans *x, t_symbol *track_sym, long bar_to_keep) {
    if (!dictionary_hasentry(x->building, track_sym)) return;

    t_atom track_dict_atom;
    dictionary_getatom(x->building, track_sym, &track_dict_atom);
    t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(track_dict, &num_keys, &keys);
    if (!keys) return;

    t_atomarray *shared_span_array = NULL;
    t_symbol **bars_to_flush_syms = (t_symbol **)sysmem_newptr(num_keys * sizeof(t_symbol *));
    long flush_count = 0;

    // First pass: Identify bars to flush and find the shared span array.
    for (long i = 0; i < num_keys; i++) {
        char *key_str = keys[i]->s_name;
        char *endptr;
        long val = strtol(key_str, &endptr, 10);
        if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) {
            t_atom bar_dict_atom;
            dictionary_getatom(track_dict, keys[i], &bar_dict_atom);
            t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);

            if (val != bar_to_keep) {
                bars_to_flush_syms[flush_count++] = keys[i];
            }
            if (!shared_span_array && dictionary_hasentry(bar_dict, gensym("span"))) {
                t_atom span_atom;
                dictionary_getatom(bar_dict, gensym("span"), &span_atom);
                shared_span_array = (t_atomarray *)atom_getobj(&span_atom);
            }
        }
    }

    // Output the span that is being ended.
    if (flush_count > 0) {
        post("Pruning span for track %s, keeping bar %ld", track_sym->s_name, bar_to_keep);

        long *bars_to_output_vals = (long *)sysmem_newptr(flush_count * sizeof(long));
        for(long i=0; i<flush_count; ++i) bars_to_output_vals[i] = atol(bars_to_flush_syms[i]->s_name);
        qsort(bars_to_output_vals, flush_count, sizeof(long), compare_longs);

        t_atom *output_atoms = (t_atom *)sysmem_newptr(flush_count * sizeof(t_atom));
        for(long i=0; i<flush_count; ++i) atom_setlong(output_atoms + i, bars_to_output_vals[i]);

        outlet_int(x->track_outlet, atol(track_sym->s_name));
        outlet_list(x->span_outlet, NULL, flush_count, output_atoms);

        sysmem_freeptr(bars_to_output_vals);
        sysmem_freeptr(output_atoms);
    }

    // Second pass: Unlink the shared span from ALL bars to prevent double-freeing.
    if (shared_span_array) {
        for (long i = 0; i < num_keys; i++) {
            char *key_str = keys[i]->s_name;
            char *endptr;
            strtol(key_str, &endptr, 10);
            if (*endptr == '\0' && (*key_str != '-' || endptr != key_str + 1)) {
                t_atom bar_dict_atom;
                dictionary_getatom(track_dict, keys[i], &bar_dict_atom);
                t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
                if (dictionary_hasentry(bar_dict, gensym("span"))) {
                    dictionary_deleteentry(bar_dict, gensym("span"));
                }
            }
        }
    }

    // Third pass: Reset the bars to be flushed to standalone state before freeing them.
    for (long i = 0; i < flush_count; i++) {
        buildspans_reset_bar_to_standalone(x, track_sym, bars_to_flush_syms[i]);
    }

    // Fourth pass: Now it's safe to free the flushed bar dictionaries.
    for (long i = 0; i < flush_count; i++) {
        t_symbol *bar_sym_to_delete = bars_to_flush_syms[i];
        if (dictionary_hasentry(track_dict, bar_sym_to_delete)) {
            t_atom bar_dict_atom;
            dictionary_getatom(track_dict, bar_sym_to_delete, &bar_dict_atom);
            object_free(atom_getobj(&bar_dict_atom)); // This will no longer free the shared span
            dictionary_deleteentry(track_dict, bar_sym_to_delete);
        }
    }

    // Finally, free the now-unreferenced shared span array itself.
    if (shared_span_array) {
        object_free(shared_span_array);
    }

    sysmem_freeptr(bars_to_flush_syms);
    sysmem_freeptr(keys);

    char bar_to_keep_str[32];
    snprintf(bar_to_keep_str, 32, "%ld", bar_to_keep);
    buildspans_reset_bar_to_standalone(x, track_sym, gensym(bar_to_keep_str));

    buildspans_visualize_memory(x);
}


void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *track_sym, t_symbol *bar_sym) {
    if (!dictionary_hasentry(x->building, track_sym)) return;

    t_atom track_dict_atom;
    dictionary_getatom(x->building, track_sym, &track_dict_atom);
    t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

    if (!dictionary_hasentry(track_dict, bar_sym)) return;

    t_atom bar_dict_atom;
    dictionary_getatom(track_dict, bar_sym, &bar_dict_atom);
    t_dictionary *bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);

    // Update rating to mean
    if (dictionary_hasentry(bar_dict, gensym("mean"))) {
        t_atom mean_atom;
        dictionary_getatom(bar_dict, gensym("mean"), &mean_atom);
        double mean_val = atom_getfloat(&mean_atom);

        if (dictionary_hasentry(bar_dict, gensym("rating"))) dictionary_deleteentry(bar_dict, gensym("rating"));
        dictionary_appendfloat(bar_dict, gensym("rating"), mean_val);
        post("%s::%s::%s %.2f", track_sym->s_name, bar_sym->s_name, "rating", mean_val);
        buildspans_log_update(x, track_sym, bar_sym, gensym("rating"), 1, &mean_atom);
    }

    // Update span to be only itself
    t_atomarray *new_span_array = atomarray_new(0, NULL);
    t_atom new_bar_atom;
    atom_setlong(&new_bar_atom, atol(bar_sym->s_name));
    atomarray_appendatom(new_span_array, &new_bar_atom);

    t_atom new_span_atom;
    atom_setobj(&new_span_atom, (t_object *)new_span_array);

    if (dictionary_hasentry(bar_dict, gensym("span"))) dictionary_deleteentry(bar_dict, gensym("span"));
    dictionary_appendatom(bar_dict, gensym("span"), &new_span_atom);

    char *span_str = atomarray_to_string(new_span_array);
    if (span_str) {
        post("%s::%s::%s %s", track_sym->s_name, bar_sym->s_name, "span", span_str);
        long count; t_atom *atoms;
        atomarray_getatoms(new_span_array, &count, &atoms);
        buildspans_log_update(x, track_sym, bar_sym, gensym("span"), count, atoms);
        sysmem_freeptr(span_str);
    }
}

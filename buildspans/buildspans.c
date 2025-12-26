#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_proto.h"
#include "../shared/visualize.h"
#include <math.h>
#include <stdlib.h> // For qsort
#include <string.h> // For isdigit

// Helper function to generate a hierarchical key symbol
t_symbol* generate_hierarchical_key(t_symbol *track, t_symbol *bar, t_symbol *key) {
    char key_str[256];
    snprintf(key_str, 256, "%s::%s::%s", track->s_name, bar->s_name, key->s_name);
    return gensym(key_str);
}

// Helper function to parse a hierarchical key symbol.
// Returns 1 on success, 0 on failure.
// Pointers passed for track, bar, and key will be set to newly allocated char buffers.
// The caller is responsible for freeing these buffers.
int parse_hierarchical_key(t_symbol *hierarchical_key, char **track, char **bar, char **key) {
    const char *key_str = hierarchical_key->s_name;
    const char *first_delim = strstr(key_str, "::");
    if (!first_delim) return 0;

    const char *second_delim = strstr(first_delim + 2, "::");
    if (!second_delim) return 0;

    size_t track_len = first_delim - key_str;
    *track = (char *)sysmem_newptr(track_len + 1);
    strncpy(*track, key_str, track_len);
    (*track)[track_len] = '\0';

    size_t bar_len = second_delim - (first_delim + 2);
    *bar = (char *)sysmem_newptr(bar_len + 1);
    strncpy(*bar, first_delim + 2, bar_len);
    (*bar)[bar_len] = '\0';

    *key = (char *)sysmem_newptr(strlen(second_delim + 2) + 1);
    strcpy(*key, second_delim + 2);

    return 1;
}


// Helper function to convert an atomarray to a string for buildspans_verbose_loging
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
    void *verbose_log_outlet;
    long verbose;
} t_buildspans;

// Function prototypes
void *buildspans_new(t_symbol *s, long argc, t_atom *argv);
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
void buildspans_verbose_log(t_buildspans *x, const char *fmt, ...);
void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *track_sym, t_symbol *bar_sym);
void buildspans_finalize_and_log_span(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_array);
void buildspans_deferred_rating_check(t_buildspans *x, t_symbol *track_sym, long last_bar_timestamp);


// Helper function to send verbose log messages
void buildspans_verbose_log(t_buildspans *x, const char *fmt, ...) {
    if (x->verbose && x->verbose_log_outlet) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        outlet_anything(x->verbose_log_outlet, gensym(buf), 0, NULL);
    }
}

// Helper function to send log messages out the log outlet
void buildspans_log_update(t_buildspans *x, t_symbol *track, t_symbol *bar, t_symbol *key, long argc, t_atom *argv) {
    char log_key_str[256];
    snprintf(log_key_str, 256, "%s::%s::%s", track->s_name, bar->s_name, key->s_name);
    outlet_anything(x->log_outlet, gensym(log_key_str), argc, argv);
}


t_class *buildspans_class;

void buildspans_visualize_memory(t_buildspans *x) {
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);

    // 1. Identify all unique tracks
    long unique_track_count = 0;
    t_symbol **unique_tracks = (t_symbol **)sysmem_newptr(num_keys * sizeof(t_symbol *)); // Over-allocate
    if (keys) {
        for (long i = 0; i < num_keys; i++) {
            char *track_str, *bar_str, *prop_str;
            if (parse_hierarchical_key(keys[i], &track_str, &bar_str, &prop_str)) {
                int found = 0;
                for (long j = 0; j < unique_track_count; j++) {
                    if (strcmp(unique_tracks[j]->s_name, track_str) == 0) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    unique_tracks[unique_track_count++] = gensym(track_str);
                }
                sysmem_freeptr(track_str);
                sysmem_freeptr(bar_str);
                sysmem_freeptr(prop_str);
            }
        }
    }

    // 2. Generate JSON
    long buffer_size = 4096;
    char *json_buffer = (char *)sysmem_newptr(buffer_size);
    long offset = 0;
    offset += snprintf(json_buffer + offset, buffer_size, "{\"building\":{");

    for (long i = 0; i < unique_track_count; i++) {
        t_symbol *track_sym = unique_tracks[i];
        if (i > 0) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
        
        offset += snprintf(json_buffer + offset, buffer_size - offset, "\"%s\":{\"absolutes\":[", track_sym->s_name);

        // a. Find and sort bars for this track
        long bar_count = 0;
        long *bar_timestamps = (long *)sysmem_newptr(num_keys * sizeof(long)); // Over-allocate
        if (keys) {
            for (long j = 0; j < num_keys; j++) {
                 char *track_str, *bar_str, *prop_str;
                 if (parse_hierarchical_key(keys[j], &track_str, &bar_str, &prop_str)) {
                     if (strcmp(track_str, track_sym->s_name) == 0 && strcmp(prop_str, "mean") == 0) {
                         bar_timestamps[bar_count++] = atol(bar_str);
                     }
                     sysmem_freeptr(track_str);
                     sysmem_freeptr(bar_str);
                     sysmem_freeptr(prop_str);
                 }
            }
        }
        qsort(bar_timestamps, bar_count, sizeof(long), compare_longs);

        // b. Append absolutes
        int first_absolute = 1;
        for (long j = 0; j < bar_count; j++) {
            char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_timestamps[j]);
            t_symbol* abs_key = generate_hierarchical_key(track_sym, gensym(bar_str), gensym("absolutes"));
            if (dictionary_hasentry(x->building, abs_key)) {
                t_atom a;
                dictionary_getatom(x->building, abs_key, &a);
                t_atomarray *arr = (t_atomarray *)atom_getobj(&a);
                long ac; t_atom *av;
                atomarray_getatoms(arr, &ac, &av);
                for (long k = 0; k < ac; k++) {
                    if (!first_absolute) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                    first_absolute = 0;
                    offset += snprintf(json_buffer + offset, buffer_size - offset, "%.2f", atom_getfloat(av+k));
                }
            }
        }
        offset += snprintf(json_buffer + offset, buffer_size - offset, "],\"offsets\":[");

        // c. Append offsets
        int first_offset = 1;
        for (long j = 0; j < bar_count; j++) {
            char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_timestamps[j]);
            t_symbol* offset_key = generate_hierarchical_key(track_sym, gensym(bar_str), gensym("offset"));
             if (dictionary_hasentry(x->building, offset_key)) {
                 if (!first_offset) offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                 first_offset = 0;
                 t_atom a;
                 dictionary_getatom(x->building, offset_key, &a);
                 offset += snprintf(json_buffer + offset, buffer_size - offset, "%.2f", atom_getfloat(&a));
             }
        }
        offset += snprintf(json_buffer + offset, buffer_size - offset, "]}");
        sysmem_freeptr(bar_timestamps);
    }
    offset += snprintf(json_buffer + offset, buffer_size - offset, "},\"current_offset\":%.2f}", x->current_offset);

    visualize(json_buffer);
    sysmem_freeptr(json_buffer);
    sysmem_freeptr(unique_tracks);
    if(keys) sysmem_freeptr(keys);
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
    
    CLASS_ATTR_LONG(c, "verbose", 0, t_buildspans, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");
    CLASS_ATTR_DEFAULT(c, "verbose", 0, "0");

    class_register(CLASS_BOX, c);
    buildspans_class = c;
}

void *buildspans_new(t_symbol *s, long argc, t_atom *argv) {
    t_buildspans *x = (t_buildspans *)object_alloc(buildspans_class);
    if (x) {
        x->building = dictionary_new();
        x->current_track = 0;
        x->current_offset = 0.0;
        x->bar_length = 125; // Default bar length
        x->current_palette = gensym("");
        x->verbose_log_outlet = NULL;

        // Process attributes before creating outlets
        attr_args_process(x, argc, argv);

        // Inlets are created from right to left.
        proxy_new((t_object *)x, 4, NULL); // Palette
        intin((t_object *)x, 3);    // Bar Length
        intin((t_object *)x, 2);    // Track Number
        floatin((t_object *)x, 1);  // Offset

        // Outlets are created from right to left
        if (x->verbose) {
            x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        }
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
    buildspans_verbose_log(x, "buildspans cleared.");
    buildspans_visualize_memory(x);
}

// Handler for float messages on the 2nd inlet (proxy #1, offset)
void buildspans_offset(t_buildspans *x, double f) {
    x->current_offset = f;
    buildspans_verbose_log(x, "Global offset updated to: %.2f", f);
    buildspans_visualize_memory(x);
}

// Handler for int messages on the 4th inlet (proxy #3, bar length)
void buildspans_bar_length(t_buildspans *x, long n) {
    x->bar_length = n;
    buildspans_verbose_log(x, "Bar length updated to: %ld", n);
}

// Handler for int messages on the 3rd inlet (proxy #2, track number)
void buildspans_track(t_buildspans *x, long n) {
    x->current_track = n;
    buildspans_verbose_log(x, "Track updated to: %ld", n);
}

// Handler for various messages, including palette symbol
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet_num = proxy_getinlet((t_object *)x);

    // Inlet 4 is the palette symbol inlet
    if (inlet_num == 4) {
        // A standalone symbol is a message with argc=0
        if (argc == 0) {
            x->current_palette = s;
            buildspans_verbose_log(x, "Palette set to: %s", s->s_name);
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

    buildspans_verbose_log(x, "--- New Timestamp-Score Pair Received ---");
    buildspans_verbose_log(x, "Absolute timestamp: %.2f, Score: %.2f", timestamp, score);

    // Get current track symbol
    char track_str[64];
    snprintf(track_str, 64, "%ld-%.0f", x->current_track, x->current_offset);
    t_symbol *track_sym = gensym(track_str);

    // Calculate bar timestamp
    double relative_timestamp = timestamp - x->current_offset;
    buildspans_verbose_log(x, "Relative timestamp (absolutes - offset): %.2f", relative_timestamp);
    long bar_timestamp_val = floor(relative_timestamp / x->bar_length) * x->bar_length;
    buildspans_verbose_log(x, "Calculated bar timestamp (rounded down to nearest %ld): %ld", x->bar_length, bar_timestamp_val);

    // --- Find the most recent bar for the current track to see if this is a new bar ---
    long last_bar_timestamp = -1;
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (keys) {
        for (long i = 0; i < num_keys; i++) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0) {
                    long bar_val = atol(key_bar);
                    if (last_bar_timestamp == -1 || bar_val > last_bar_timestamp) {
                        last_bar_timestamp = bar_val;
                    }
                }
                sysmem_freeptr(key_track);
                sysmem_freeptr(key_bar);
                sysmem_freeptr(key_prop);
            }
        }
        sysmem_freeptr(keys);
    }

    int is_new_bar = (last_bar_timestamp == -1 || bar_timestamp_val > last_bar_timestamp);

    // --- Deferred span ending logic (only if a new bar is detected) ---
    if (is_new_bar && last_bar_timestamp != -1) {
        buildspans_deferred_rating_check(x, track_sym, last_bar_timestamp);

        // Discontiguity check: find the most recent bar for this track AGAIN
        // as the deferred check might have pruned some bars.
        long most_recent_bar_after_rating_check = -1;
        dictionary_getkeys(x->building, &num_keys, &keys);
        if (keys) {
            for (long i = 0; i < num_keys; i++) {
                char *key_track, *key_bar, *key_prop;
                if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
                    if (strcmp(key_track, track_sym->s_name) == 0) {
                        long bar_val = atol(key_bar);
                        if (most_recent_bar_after_rating_check == -1 || bar_val > most_recent_bar_after_rating_check) {
                            most_recent_bar_after_rating_check = bar_val;
                        }
                    }
                    sysmem_freeptr(key_track);
                    sysmem_freeptr(key_bar);
                    sysmem_freeptr(key_prop);
                }
            }
            sysmem_freeptr(keys);
        }

        if (most_recent_bar_after_rating_check != -1 && bar_timestamp_val > most_recent_bar_after_rating_check + x->bar_length) {
            buildspans_verbose_log(x, "Discontiguous bar detected. New bar %ld is more than %ldms after last bar %ld.", bar_timestamp_val, x->bar_length, most_recent_bar_after_rating_check);
            buildspans_end_track_span(x, track_sym);
        }
    }

    // --- ADD OR UPDATE BAR ---
    char bar_str[32];
    snprintf(bar_str, 32, "%ld", bar_timestamp_val);
    t_symbol *bar_sym = gensym(bar_str);

    // Update offset
    t_symbol *offset_key = generate_hierarchical_key(track_sym, bar_sym, gensym("offset"));
    if (dictionary_hasentry(x->building, offset_key)) dictionary_deleteentry(x->building, offset_key);
    dictionary_appendfloat(x->building, offset_key, x->current_offset);
    buildspans_verbose_log(x, "%s::%s %.2f", offset_key->s_name, "offset", x->current_offset);
    t_atom offset_atom;
    atom_setfloat(&offset_atom, x->current_offset);
    buildspans_log_update(x, track_sym, bar_sym, gensym("offset"), 1, &offset_atom);

    // Update palette
    t_symbol *palette_key = generate_hierarchical_key(track_sym, bar_sym, gensym("palette"));
    if (dictionary_hasentry(x->building, palette_key)) dictionary_deleteentry(x->building, palette_key);
    dictionary_appendsym(x->building, palette_key, x->current_palette);
    buildspans_verbose_log(x, "%s::%s %s", palette_key->s_name, "palette", x->current_palette->s_name);
    t_atom palette_atom;
    atom_setsym(&palette_atom, x->current_palette);
    buildspans_log_update(x, track_sym, bar_sym, gensym("palette"), 1, &palette_atom);

    // Update absolutes array
    t_symbol *absolutes_key = generate_hierarchical_key(track_sym, bar_sym, gensym("absolutes"));
    t_atomarray *absolutes_array;
    if (!dictionary_hasentry(x->building, absolutes_key)) {
        absolutes_array = atomarray_new(0, NULL);
        t_atom a; atom_setobj(&a, (t_object *)absolutes_array);
        dictionary_appendatom(x->building, absolutes_key, &a);
    } else {
        t_atom a; dictionary_getatom(x->building, absolutes_key, &a);
        absolutes_array = (t_atomarray *)atom_getobj(&a);
    }
    t_atom new_absolute; atom_setfloat(&new_absolute, timestamp);
    atomarray_appendatom(absolutes_array, &new_absolute);
    char *abs_str = atomarray_to_string(absolutes_array);
    if (abs_str) {
        buildspans_verbose_log(x, "%s %s", absolutes_key->s_name, abs_str);
        sysmem_freeptr(abs_str);
        long count; t_atom *atoms;
        atomarray_getatoms(absolutes_array, &count, &atoms);
        buildspans_log_update(x, track_sym, bar_sym, gensym("absolutes"), count, atoms);
    }

    // Update scores array
    t_symbol *scores_key = generate_hierarchical_key(track_sym, bar_sym, gensym("scores"));
    t_atomarray *scores_array;
    if (!dictionary_hasentry(x->building, scores_key)) {
        scores_array = atomarray_new(0, NULL);
        t_atom a; atom_setobj(&a, (t_object *)scores_array);
        dictionary_appendatom(x->building, scores_key, &a);
    } else {
        t_atom a; dictionary_getatom(x->building, scores_key, &a);
        scores_array = (t_atomarray *)atom_getobj(&a);
    }
    t_atom new_score; atom_setfloat(&new_score, score);
    atomarray_appendatom(scores_array, &new_score);
    char *scores_str = atomarray_to_string(scores_array);
    if (scores_str) {
        buildspans_verbose_log(x, "%s %s", scores_key->s_name, scores_str);
        sysmem_freeptr(scores_str);
        long count; t_atom *atoms;
        atomarray_getatoms(scores_array, &count, &atoms);
        buildspans_log_update(x, track_sym, bar_sym, gensym("scores"), count, atoms);
    }

    // Update mean
    long scores_count = atomarray_getsize(scores_array);
    if (scores_count > 0) {
        double sum = 0.0;
        long count; t_atom *atoms;
        atomarray_getatoms(scores_array, &count, &atoms);
        for (long i = 0; i < count; i++) sum += atom_getfloat(&atoms[i]);
        double mean = sum / count;

        t_symbol *mean_key = generate_hierarchical_key(track_sym, bar_sym, gensym("mean"));
        if (dictionary_hasentry(x->building, mean_key)) dictionary_deleteentry(x->building, mean_key);
        dictionary_appendfloat(x->building, mean_key, mean);
        buildspans_verbose_log(x, "%s %.2f", mean_key->s_name, mean);
        t_atom mean_atom;
        atom_setfloat(&mean_atom, mean);
        buildspans_log_update(x, track_sym, bar_sym, gensym("mean"), 1, &mean_atom);
    }

    // --- UPDATE AND BACK-PROPAGATE SPAN ---
    // 1. Get all bar timestamps for the current track.
    dictionary_getkeys(x->building, &num_keys, &keys);
    long bar_timestamps_count = 0;
    long *bar_timestamps = (long *)sysmem_newptr(num_keys * sizeof(long)); // Over-allocate, then count
    if (keys) {
        for (long i = 0; i < num_keys; i++) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0) {
                    // To avoid duplicates, only add when we see the 'mean' property
                    if (strcmp(key_prop, "mean") == 0) {
                        bar_timestamps[bar_timestamps_count++] = atol(key_bar);
                    }
                }
                sysmem_freeptr(key_track);
                sysmem_freeptr(key_bar);
                sysmem_freeptr(key_prop);
            }
        }
        sysmem_freeptr(keys);
    }
    qsort(bar_timestamps, bar_timestamps_count, sizeof(long), compare_longs);

    // 2. Create the NEW span array and link it to all bars in the track.
    t_atomarray *new_span_array = atomarray_new(0, NULL);
    for (long i = 0; i < bar_timestamps_count; i++) {
        t_atom a; atom_setlong(&a, bar_timestamps[i]);
        atomarray_appendatom(new_span_array, &a);
    }
    t_atom new_span_atom;
    atom_setobj(&new_span_atom, (t_object *)new_span_array);

    char *span_str = atomarray_to_string(new_span_array);
    if (span_str) {
        for (long i = 0; i < bar_timestamps_count; i++) {
            char temp_bar_str[32]; snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);
            t_symbol *span_key = generate_hierarchical_key(track_sym, temp_bar_sym, gensym("span"));
            
            // dictionary_appendatom with an object replaces the old one and handles refcount
            dictionary_appendatom(x->building, span_key, &new_span_atom);

            // Logging
            buildspans_verbose_log(x, "%s %s", span_key->s_name, span_str);
            long count; t_atom *atoms;
            atomarray_getatoms(new_span_array, &count, &atoms);
            buildspans_log_update(x, track_sym, temp_bar_sym, gensym("span"), count, atoms);
        }
        sysmem_freeptr(span_str);
    }

    // --- RATING CALCULATION & BACK-PROPAGATION ---
    double final_lowest_mean = -1.0;
    for (long i = 0; i < bar_timestamps_count; i++) {
        char temp_bar_str[32]; snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
        t_symbol *temp_bar_sym = gensym(temp_bar_str);
        t_symbol *mean_key = generate_hierarchical_key(track_sym, temp_bar_sym, gensym("mean"));
        if (dictionary_hasentry(x->building, mean_key)) {
            t_atom mean_atom;
            dictionary_getatom(x->building, mean_key, &mean_atom);
            double current_bar_mean = atom_getfloat(&mean_atom);
            if (final_lowest_mean == -1.0 || current_bar_mean < final_lowest_mean) {
                final_lowest_mean = current_bar_mean;
            }
        }
    }
    if (final_lowest_mean != -1.0) {
        double final_rating = final_lowest_mean * bar_timestamps_count;
        for (long i = 0; i < bar_timestamps_count; i++) {
            char temp_bar_str[32]; snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);
            t_symbol *rating_key = generate_hierarchical_key(track_sym, temp_bar_sym, gensym("rating"));
            if (dictionary_hasentry(x->building, rating_key)) dictionary_deleteentry(x->building, rating_key);
            dictionary_appendfloat(x->building, rating_key, final_rating);
            buildspans_verbose_log(x, "%s %.2f", rating_key->s_name, final_rating);
            t_atom rating_atom;
            atom_setfloat(&rating_atom, final_rating);
            buildspans_log_update(x, track_sym, temp_bar_sym, gensym("rating"), 1, &rating_atom);
        }
        buildspans_verbose_log(x, "Final rating for span: %.2f (%.2f * %ld)", final_rating, final_lowest_mean, bar_timestamps_count);
    }

    sysmem_freeptr(bar_timestamps);
    buildspans_visualize_memory(x);
}

void buildspans_end_track_span(t_buildspans *x, t_symbol *track_sym) {
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return;

    // 1. Find the first 'span' atomarray for the track. They are all shared.
    t_atomarray *span_to_output = NULL;
    if (keys) {
        for (long i = 0; i < num_keys; i++) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_prop, "span") == 0) {
                    t_atom a;
                    dictionary_getatom(x->building, keys[i], &a);
                    span_to_output = (t_atomarray *)atom_getobj(&a);
                    sysmem_freeptr(key_track);
                    sysmem_freeptr(key_bar);
                    sysmem_freeptr(key_prop);
                    break; // Found it.
                }
                sysmem_freeptr(key_track);
                sysmem_freeptr(key_bar);
                sysmem_freeptr(key_prop);
            }
        }
    }

    // 2. If no span was found (e.g., single bar), create one locally.
    int local_span_created = 0;
    if (!span_to_output && keys) {
        local_span_created = 1;
        span_to_output = atomarray_new(0, NULL);
        long bar_count = 0;
        long *bar_timestamps = (long *)sysmem_newptr(num_keys * sizeof(long));
        for (long i = 0; i < num_keys; i++) {
            char *key_track, *key_bar, *key_prop;
             if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_prop, "mean") == 0) {
                     bar_timestamps[bar_count++] = atol(key_bar);
                }
                sysmem_freeptr(key_track);
                sysmem_freeptr(key_bar);
                sysmem_freeptr(key_prop);
            }
        }
        qsort(bar_timestamps, bar_count, sizeof(long), compare_longs);
        for(long i=0; i<bar_count; ++i) {
            t_atom a;
            atom_setlong(&a, bar_timestamps[i]);
            atomarray_appendatom(span_to_output, &a);
        }
        sysmem_freeptr(bar_timestamps);
    }


    // 3. Calculate rating and output.
    if (span_to_output) {
        long span_size;
        t_atom *span_atoms;
        atomarray_getatoms(span_to_output, &span_size, &span_atoms);

        double lowest_mean = -1.0;
        if (span_size > 0) {
             for (long i = 0; i < span_size; i++) {
                long bar_ts = atom_getlong(&span_atoms[i]);
                char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_ts);
                t_symbol *bar_sym = gensym(bar_str);
                t_symbol *mean_key = generate_hierarchical_key(track_sym, bar_sym, gensym("mean"));
                if (dictionary_hasentry(x->building, mean_key)) {
                    t_atom mean_atom;
                    dictionary_getatom(x->building, mean_key, &mean_atom);
                    double bar_mean = atom_getfloat(&mean_atom);
                    if (lowest_mean == -1.0 || bar_mean < lowest_mean) {
                        lowest_mean = bar_mean;
                    }
                }
            }
        }
        double final_rating = (lowest_mean != -1.0) ? (lowest_mean * span_size) : 0.0;
        buildspans_verbose_log(x, "Ending span for track %s with rating %.2f (%.2f * %ld)", track_sym->s_name, final_rating, lowest_mean, span_size);

        long track_num_to_output;
        sscanf(track_sym->s_name, "%ld-", &track_num_to_output);
        outlet_int(x->track_outlet, track_num_to_output);
        outlet_list(x->span_outlet, NULL, span_size, span_atoms);

        if (local_span_created) {
            object_free(span_to_output);
        }
    }

    // 4. Delete all keys associated with the track.
    if (keys) {
        t_symbol **keys_to_delete = (t_symbol**)sysmem_newptr(num_keys * sizeof(t_symbol*));
        long delete_count = 0;
        for(long i=0; i<num_keys; ++i) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
                 if (strcmp(key_track, track_sym->s_name) == 0) {
                     keys_to_delete[delete_count++] = keys[i];
                 }
                 sysmem_freeptr(key_track);
                 sysmem_freeptr(key_bar);
                 sysmem_freeptr(key_prop);
             }
        }
        for(long i=0; i<delete_count; ++i) {
            dictionary_deleteentry(x->building, keys_to_delete[i]);
        }
        sysmem_freeptr(keys_to_delete);
        sysmem_freeptr(keys);
    }

    buildspans_visualize_memory(x);
}


void buildspans_bang(t_buildspans *x) {
    buildspans_verbose_log(x, "Flush triggered by bang.");
    buildspans_flush(x);
}

void buildspans_flush(t_buildspans *x) {
    long num_tracks;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_tracks, &keys);
    if (!keys) return;

    // 1. Identify all unique tracks.
    long track_count = 0;
    t_symbol **track_syms = (t_symbol **)sysmem_newptr(num_tracks * sizeof(t_symbol *));
    for (long i = 0; i < num_tracks; i++) {
        char *key_track, *key_bar, *key_prop;
        if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
            int found = 0;
            for(long j=0; j<track_count; ++j) {
                if (strcmp(track_syms[j]->s_name, key_track) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                track_syms[track_count++] = gensym(key_track);
            }
            sysmem_freeptr(key_track);
            sysmem_freeptr(key_bar);
            sysmem_freeptr(key_prop);
        }
    }

    // 2. For each track, perform deferred rating check and end the span.
    for (long i = 0; i < track_count; i++) {
        t_symbol *track_sym = track_syms[i];

        // Find the last bar for this track.
        long last_bar_timestamp = -1;
        for (long j = 0; j < num_tracks; j++) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[j], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0) {
                    long bar_val = atol(key_bar);
                    if (last_bar_timestamp == -1 || bar_val > last_bar_timestamp) {
                        last_bar_timestamp = bar_val;
                    }
                }
                sysmem_freeptr(key_track);
                sysmem_freeptr(key_bar);
                sysmem_freeptr(key_prop);
            }
        }

        if (last_bar_timestamp != -1) {
            buildspans_deferred_rating_check(x, track_sym, last_bar_timestamp);
        }

        buildspans_end_track_span(x, track_sym);
    }

    sysmem_freeptr(track_syms);
    sysmem_freeptr(keys);
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
        if (x->verbose) {
            switch (a) {
                case 0: sprintf(s, "Span Data (list)"); break;
                case 1: sprintf(s, "Track Number (int)"); break;
                case 2: sprintf(s, "Sync Outlet (anything)"); break;
                case 3: sprintf(s, "Verbose Logging Outlet"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "Span Data (list)"); break;
                case 1: sprintf(s, "Track Number (int)"); break;
                case 2: sprintf(s, "Sync Outlet (anything)"); break;
            }
        }
    }
}

void buildspans_prune_span(t_buildspans *x, t_symbol *track_sym, long bar_to_keep) {
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return;

    // 1. Identify all bars to flush.
    long flush_count = 0;
    long *bars_to_flush_vals = (long *)sysmem_newptr(num_keys * sizeof(long));

    for (long i = 0; i < num_keys; i++) {
        char *key_track, *key_bar, *key_prop;
        if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
            if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_prop, "mean") == 0) {
                long bar_val = atol(key_bar);
                if (bar_val != bar_to_keep) {
                    bars_to_flush_vals[flush_count++] = bar_val;
                }
            }
            sysmem_freeptr(key_track);
            sysmem_freeptr(key_bar);
            sysmem_freeptr(key_prop);
        }
    }

    // 2. Output the flushed span.
    if (flush_count > 0) {
        buildspans_verbose_log(x, "Pruning span for track %s, keeping bar %ld", track_sym->s_name, bar_to_keep);
        qsort(bars_to_flush_vals, flush_count, sizeof(long), compare_longs);
        t_atom *output_atoms = (t_atom *)sysmem_newptr(flush_count * sizeof(t_atom));
        for(long i=0; i<flush_count; ++i) atom_setlong(output_atoms + i, bars_to_flush_vals[i]);
        long track_num_to_output;
        sscanf(track_sym->s_name, "%ld-", &track_num_to_output);
        outlet_int(x->track_outlet, track_num_to_output);
        outlet_list(x->span_outlet, NULL, flush_count, output_atoms);
        sysmem_freeptr(output_atoms);
    }
    
    // 3. Finalize the state of the flushed bars.
    if (flush_count > 0) {
        buildspans_verbose_log(x, "Finalizing flushed span...");
        t_atomarray *flushed_span_array = atomarray_new(0, NULL);
        for(long i = 0; i < flush_count; ++i) {
            t_atom a; atom_setlong(&a, bars_to_flush_vals[i]);
            atomarray_appendatom(flushed_span_array, &a);
        }
        buildspans_finalize_and_log_span(x, track_sym, flushed_span_array);
        object_free(flushed_span_array);
    }

    // 4. Delete keys for flushed bars.
    t_symbol **keys_to_delete = (t_symbol**)sysmem_newptr(num_keys * sizeof(t_symbol*));
    long delete_count = 0;
    for (long i = 0; i < flush_count; i++) {
        char bar_str[32];
        snprintf(bar_str, 32, "%ld", bars_to_flush_vals[i]);
        for(long j=0; j<num_keys; ++j) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[j], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_bar, bar_str) == 0) {
                     keys_to_delete[delete_count++] = keys[j];
                }
                sysmem_freeptr(key_track);
                sysmem_freeptr(key_bar);
                sysmem_freeptr(key_prop);
            }
        }
    }
    for(long i=0; i<delete_count; ++i) {
        dictionary_deleteentry(x->building, keys_to_delete[i]);
    }

    sysmem_freeptr(keys_to_delete);
    sysmem_freeptr(bars_to_flush_vals);
    sysmem_freeptr(keys);

    char bar_to_keep_str[32];
    snprintf(bar_to_keep_str, 32, "%ld", bar_to_keep);
    buildspans_verbose_log(x, "Resetting kept bar...");
    buildspans_reset_bar_to_standalone(x, track_sym, gensym(bar_to_keep_str));
    buildspans_visualize_memory(x);
}


void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *track_sym, t_symbol *bar_sym) {
    // Update rating to mean
    t_symbol *mean_key = generate_hierarchical_key(track_sym, bar_sym, gensym("mean"));
    if (dictionary_hasentry(x->building, mean_key)) {
        t_atom mean_atom;
        dictionary_getatom(x->building, mean_key, &mean_atom);
        double mean_val = atom_getfloat(&mean_atom);

        t_symbol *rating_key = generate_hierarchical_key(track_sym, bar_sym, gensym("rating"));
        if (dictionary_hasentry(x->building, rating_key)) dictionary_deleteentry(x->building, rating_key);
        dictionary_appendfloat(x->building, rating_key, mean_val);
        buildspans_verbose_log(x, "%s %.2f", rating_key->s_name, mean_val);
        buildspans_log_update(x, track_sym, bar_sym, gensym("rating"), 1, &mean_atom);
    }

    // Update span to be only itself
    t_atomarray *new_span_array = atomarray_new(0, NULL);
    t_atom new_bar_atom;
    atom_setlong(&new_bar_atom, atol(bar_sym->s_name));
    atomarray_appendatom(new_span_array, &new_bar_atom);

    t_atom new_span_atom;
    atom_setobj(&new_span_atom, (t_object *)new_span_array);

    t_symbol *span_key = generate_hierarchical_key(track_sym, bar_sym, gensym("span"));
    dictionary_appendatom(x->building, span_key, &new_span_atom);

    char *span_str = atomarray_to_string(new_span_array);
    if (span_str) {
        buildspans_verbose_log(x, "%s %s", span_key->s_name, span_str);
        long count; t_atom *atoms;
        atomarray_getatoms(new_span_array, &count, &atoms);
        buildspans_log_update(x, track_sym, bar_sym, gensym("span"), count, atoms);
        sysmem_freeptr(span_str);
    }
}


void buildspans_finalize_and_log_span(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_array) {
    if (!span_array) return;

    long span_size = 0;
    t_atom *span_atoms = NULL;
    atomarray_getatoms(span_array, &span_size, &span_atoms);
    if (span_size == 0) return;

    // 1. Calculate the final rating for this span.
    double lowest_mean = -1.0;
    for (long i = 0; i < span_size; i++) {
        long bar_ts = atom_getlong(&span_atoms[i]);
        char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_ts);
        t_symbol *bar_sym = gensym(bar_str);
        t_symbol *mean_key = generate_hierarchical_key(track_sym, bar_sym, gensym("mean"));
        if (dictionary_hasentry(x->building, mean_key)) {
            t_atom mean_atom;
            dictionary_getatom(x->building, mean_key, &mean_atom);
            double bar_mean = atom_getfloat(&mean_atom);
            if (lowest_mean == -1.0 || bar_mean < lowest_mean) {
                lowest_mean = bar_mean;
            }
        }
    }
    double final_rating = (lowest_mean != -1.0) ? (lowest_mean * span_size) : 0.0;

    // 2. Back-propagate the final rating and span to all bars in the span.
    t_atom rating_atom;
    atom_setfloat(&rating_atom, final_rating);
    t_atom span_atom;
    atom_setobj(&span_atom, (t_object *)span_array); // This is a shared object
    char *span_str = atomarray_to_string(span_array);

    for (long i = 0; i < span_size; i++) {
        long bar_ts = atom_getlong(&span_atoms[i]);
        char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_ts);
        t_symbol *bar_sym = gensym(bar_str);

        t_symbol *rating_key = generate_hierarchical_key(track_sym, bar_sym, gensym("rating"));
        if(dictionary_hasentry(x->building, rating_key)) dictionary_deleteentry(x->building, rating_key);
        dictionary_appendatom(x->building, rating_key, &rating_atom);
        buildspans_verbose_log(x, "%s %.2f", rating_key->s_name, final_rating);
        buildspans_log_update(x, track_sym, bar_sym, gensym("rating"), 1, &rating_atom);

        t_symbol *span_key = generate_hierarchical_key(track_sym, bar_sym, gensym("span"));
        dictionary_appendatom(x->building, span_key, &span_atom);
        if (span_str) {
            buildspans_verbose_log(x, "%s %s", span_key->s_name, span_str);
            buildspans_log_update(x, track_sym, bar_sym, gensym("span"), span_size, span_atoms);
        }
    }
    if (span_str) sysmem_freeptr(span_str);
}


void buildspans_deferred_rating_check(t_buildspans *x, t_symbol *track_sym, long last_bar_timestamp) {
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return;

    // 1. Get all bars for the track
    long bar_count = 0;
    long *bar_timestamps = (long *)sysmem_newptr(num_keys * sizeof(long));
    for (long i = 0; i < num_keys; i++) {
        char *key_track, *key_bar, *key_prop;
        if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
            if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_prop, "mean") == 0) {
                bar_timestamps[bar_count++] = atol(key_bar);
            }
            sysmem_freeptr(key_track);
            sysmem_freeptr(key_bar);
            sysmem_freeptr(key_prop);
        }
    }
    
    int prune_span = 0;
    if (bar_count > 1) {
        // 2. RATING WITH LAST BAR
        double lowest_mean_with = -1.0;
        for (long i = 0; i < bar_count; i++) {
            char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *bar_sym = gensym(bar_str);
            t_symbol *mean_key = generate_hierarchical_key(track_sym, bar_sym, gensym("mean"));
            if (dictionary_hasentry(x->building, mean_key)) {
                t_atom mean_atom;
                dictionary_getatom(x->building, mean_key, &mean_atom);
                double bar_mean = atom_getfloat(&mean_atom);
                if (lowest_mean_with == -1.0 || bar_mean < lowest_mean_with) {
                    lowest_mean_with = bar_mean;
                }
            }
        }
        double rating_with = lowest_mean_with * bar_count;

        // 3. RATING WITHOUT LAST BAR
        double lowest_mean_without = -1.0;
        long bars_without_count = 0;
        double last_bar_mean = 0.0;
        for (long i = 0; i < bar_count; i++) {
             char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *bar_sym = gensym(bar_str);
            t_symbol *mean_key = generate_hierarchical_key(track_sym, bar_sym, gensym("mean"));
            if (dictionary_hasentry(x->building, mean_key)) {
                t_atom mean_atom;
                dictionary_getatom(x->building, mean_key, &mean_atom);
                double bar_mean = atom_getfloat(&mean_atom);
                if (bar_timestamps[i] == last_bar_timestamp) {
                    last_bar_mean = bar_mean;
                } else {
                     if (lowest_mean_without == -1.0 || bar_mean < lowest_mean_without) {
                        lowest_mean_without = bar_mean;
                    }
                    bars_without_count++;
                }
            }
        }
        double rating_without = (bars_without_count > 0) ? (lowest_mean_without * bars_without_count) : 0.0;
        
        if (rating_with < rating_without) {
            buildspans_verbose_log(x, "Deferred rating check: Including bar %ld decreased rating (%.2f -> %.2f). Pruning span.", last_bar_timestamp, rating_without, rating_with);
            prune_span = 1;
        } else if (last_bar_mean > rating_with) {
            buildspans_verbose_log(x, "Deferred rating check: Bar %ld's individual rating (%.2f) is higher than span rating if included (%.2f). Pruning span.", last_bar_timestamp, last_bar_mean, rating_with);
            prune_span = 1;
        } else {
            buildspans_verbose_log(x, "Deferred rating check: Including bar %ld did not decrease rating (%.2f -> %.2f) and is not better on its own. Continuing span.", last_bar_timestamp, rating_without, rating_with);
        }

        if (prune_span) {
            buildspans_prune_span(x, track_sym, last_bar_timestamp);
        }
    }
    
    sysmem_freeptr(bar_timestamps);
    sysmem_freeptr(keys);
}

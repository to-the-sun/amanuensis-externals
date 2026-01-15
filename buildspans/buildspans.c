#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_proto.h"
#include "ext_buffer.h"
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


// Helper function to create a deep copy of a t_atomarray
t_atomarray* atomarray_deep_copy(t_atomarray *src) {
    if (!src) {
        return NULL;
    }
    long count;
    t_atom *atoms;
    atomarray_getatoms(src, &count, &atoms);

    // atomarray_new with arguments creates a new array and copies the atoms from the source.
    t_atomarray *dest = atomarray_new(count, atoms);
    return dest;
}


// Comparison function for qsort
int compare_longs(const void *a, const void *b) {
    long la = *(const long *)a;
    long lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

// Struct for holding note pairs for sorting
typedef struct { double timestamp; double score; } NotePair;

// Comparison function for qsort to sort NotePairs
int compare_notepairs(const void *a, const void *b) {
    NotePair *pa = (NotePair *)a;
    NotePair *pb = (NotePair *)b;
    if (pa->timestamp < pb->timestamp) return -1;
    if (pa->timestamp > pb->timestamp) return 1;
    return 0;
}

// Struct for holding note data for duplication manifest
typedef struct {
    long track_number;
    double timestamp;
    double score;
} t_duplication_manifest_item;

// Comparison function for qsort to sort t_duplication_manifest_item by timestamp
int compare_manifest_items(const void *a, const void *b) {
    t_duplication_manifest_item *pa = (t_duplication_manifest_item *)a;
    t_duplication_manifest_item *pb = (t_duplication_manifest_item *)b;
    if (pa->timestamp < pb->timestamp) return -1;
    if (pa->timestamp > pb->timestamp) return 1;
    return 0;
}


typedef struct _buildspans {
    t_object s_obj;
    t_dictionary *building;
    t_dictionary *tracks_ended_in_current_event;
    long current_track;
    long current_offset;
    t_buffer_ref *buffer_ref;
    t_symbol *s_buffer_name;
    t_symbol *current_palette;
    void *span_outlet;
    void *track_outlet;
    void *log_outlet;
    void *verbose_log_outlet;
    long verbose;
    double local_bar_length;
} t_buildspans;

// Function prototypes
void *buildspans_new(t_symbol *s, long argc, t_atom *argv);
void buildspans_free(t_buildspans *x);
void buildspans_clear(t_buildspans *x);
void buildspans_list(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_offset(t_buildspans *x, double f);
void buildspans_track(t_buildspans *x, long n);
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv);
void buildspans_assist(t_buildspans *x, void *b, long m, long a, char *s);
void buildspans_bang(t_buildspans *x);
void buildspans_flush(t_buildspans *x);
void buildspans_end_track_span(t_buildspans *x, t_symbol *track_sym);
void buildspans_prune_span(t_buildspans *x, t_symbol *track_sym, long bar_to_keep);
void buildspans_visualize_memory(t_buildspans *x);
void buildspans_verbose_log(t_buildspans *x, const char *fmt, ...);
void buildspans_reset_bar_to_standalone(t_buildspans *x, t_symbol *track_sym, t_symbol *bar_sym);
void buildspans_finalize_and_log_span(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_array);
void buildspans_deferred_rating_check(t_buildspans *x, t_symbol *track_sym, long last_bar_timestamp);
void buildspans_process_and_add_note(t_buildspans *x, double timestamp, double score, long offset, long bar_length);
void buildspans_cleanup_track_offset_if_needed(t_buildspans *x, t_symbol *track_offset_sym);
long find_next_offset(t_buildspans *x, long track_num_to_check, long offset_val_to_check);
int buildspans_validate_span_before_output(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_to_output);
void buildspans_output_span_data(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_atom_array);
long buildspans_get_bar_length(t_buildspans *x);
void buildspans_set_bar_buffer(t_buildspans *x, t_symbol *s);
void buildspans_local_bar_length(t_buildspans *x, double f);


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

t_class *buildspans_class;

// A single, consolidated function to get the bar length from the buffer.
// It returns -1 on any failure (buffer not set, not found, empty, or value <= 0).
// It does not post any messages to the console. The caller is responsible for that.
long buildspans_get_bar_length(t_buildspans *x) {
    if (x->local_bar_length > 0) {
        return (long)x->local_bar_length;
    }
    if (!x->buffer_ref) {
        return -1; // Buffer name not set.
    }

    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        return -1; // Buffer object not found.
    }

    long bar_length = 0;
    float *samples = buffer_locksamples(b);
    if (samples) {
        if (buffer_getframecount(b) > 0) {
            bar_length = (long)samples[0];
        }
        buffer_unlocksamples(b);
    }

    if (bar_length <= 0) {
        return -1; // Value in buffer is not positive.
    }

    return bar_length;
}

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
                 offset += snprintf(json_buffer + offset, buffer_size - offset, "%ld", atom_getlong(&a));
             }
        }
        offset += snprintf(json_buffer + offset, buffer_size - offset, "],\"span\":[");

        // d. Append span data
        int first_span = 1;
        // To avoid duplicates, find a representative span from the first bar
        if (bar_count > 0) {
            char first_bar_str[32];
            snprintf(first_bar_str, 32, "%ld", bar_timestamps[0]);
            t_symbol *span_key = generate_hierarchical_key(track_sym, gensym(first_bar_str), gensym("span"));

            if (dictionary_hasentry(x->building, span_key)) {
                t_atom a;
                dictionary_getatom(x->building, span_key, &a);
                t_atomarray *span_array = (t_atomarray *)atom_getobj(&a);
                long span_count;
                t_atom *span_atoms;
                atomarray_getatoms(span_array, &span_count, &span_atoms);

                for (long k = 0; k < span_count; k++) {
                    if (!first_span) {
                        offset += snprintf(json_buffer + offset, buffer_size - offset, ",");
                    }
                    first_span = 0;
                    offset += snprintf(json_buffer + offset, buffer_size - offset, "%ld", atom_getlong(span_atoms + k));
                }
            }
        }

        offset += snprintf(json_buffer + offset, buffer_size - offset, "]}");
        sysmem_freeptr(bar_timestamps);
    }
    long bar_length = buildspans_get_bar_length(x);
    offset += snprintf(json_buffer + offset, buffer_size - offset, "},\"current_offset\":%ld,\"bar_length\":%ld}", x->current_offset, bar_length);

    if (x->verbose && x->verbose_log_outlet) {
        visualize(json_buffer);
        // t_atom json_atom;
        // atom_setsym(&json_atom, gensym(json_buffer));
        // outlet_anything(x->verbose_log_outlet, gensym("visualize"), 1, &json_atom);
    }

    sysmem_freeptr(json_buffer);
    sysmem_freeptr(unique_tracks);
    if(keys) sysmem_freeptr(keys);
}


void ext_main(void *r) {
    t_class *c;
    c = class_new("buildspans", (method)buildspans_new, (method)buildspans_free, (short)sizeof(t_buildspans), 0L, A_GIMME, 0);
    class_addmethod(c, (method)buildspans_clear, "clear", 0);
    class_addmethod(c, (method)buildspans_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)buildspans_track, "in2", A_LONG, 0);
    class_addmethod(c, (method)buildspans_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)buildspans_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)buildspans_bang, "bang", 0);
    class_addmethod(c, (method)buildspans_set_bar_buffer, "set_bar_buffer", A_SYM, 0);
    class_addmethod(c, (method)buildspans_local_bar_length, "ft4", A_FLOAT, 0);
    
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
        x->tracks_ended_in_current_event = dictionary_new();
        buildspans_verbose_log(x, "NEW: Created building dictionary: %p", x->building);
        x->current_track = 0;
        x->current_offset = 0;
        x->current_palette = gensym("");
        x->verbose_log_outlet = NULL;
        x->buffer_ref = NULL;
        x->s_buffer_name = NULL;
        x->local_bar_length = 0;

        // Process attributes before creating outlets
        attr_args_process(x, argc, argv);

        // Hardcode the default buffer name to "bar".
        buildspans_set_bar_buffer(x, gensym("bar"));

        // Inlets are created from right to left.
        floatin((t_object *)x, 4);  // Local bar length
        proxy_new((t_object *)x, 3, NULL); // Palette
        intin((t_object *)x, 2);    // Track Number
        floatin((t_object *)x, 1);  // Offset

        // Outlets are created from right to left
        if (x->verbose) {
            x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
            visualize_init();
        }
        x->log_outlet = outlet_new((t_object *)x, NULL); // Generic outlet for logs
        x->track_outlet = intout((t_object *)x);
        x->span_outlet = outlet_new((t_object *)x, "list");
    }
    return (x);
}

void buildspans_free(t_buildspans *x) {
    if (x->verbose) {
        visualize_cleanup();
    }
    if (x->building) {
        buildspans_verbose_log(x, "FREE: Freeing building dictionary: %p", x->building);
        object_free(x->building);
    }
    if (x->tracks_ended_in_current_event) {
        object_free(x->tracks_ended_in_current_event);
    }
    if (x->buffer_ref) {
        object_free(x->buffer_ref);
    }
}

void buildspans_clear(t_buildspans *x) {
    if (x->building) {
        buildspans_verbose_log(x, "CLEAR: Freeing building dictionary: %p", x->building);
        object_free(x->building);
    }
    if (x->tracks_ended_in_current_event) {
        object_free(x->tracks_ended_in_current_event);
    }
    x->building = dictionary_new();
    x->tracks_ended_in_current_event = dictionary_new();
    buildspans_verbose_log(x, "CLEAR: Created new building dictionary: %p", x->building);
    x->current_track = 0;
    x->current_offset = 0;
    x->current_palette = gensym("");
    buildspans_verbose_log(x, "buildspans cleared.");
    buildspans_visualize_memory(x);
}

// Handler for float messages on the 2nd inlet (proxy #1, offset)
void buildspans_offset(t_buildspans *x, double f) {
    long new_offset = (long)round(f);
    // Only duplicate if the new offset is different and the old offset was not the initial default.
    if (new_offset == x->current_offset || x->current_offset == 0) {
        x->current_offset = new_offset;
        buildspans_verbose_log(x, "Global offset updated to: %ld. No duplication.", new_offset);
        return;
    }

    // Update the global offset first
    x->current_offset = new_offset;
    buildspans_verbose_log(x, "Global offset updated to: %ld. Duplicating one span for each active track.", new_offset);

    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return;

    // --- GATHER PHASE ---
    buildspans_verbose_log(x, "Gathering notes for span duplication.");
    // First, we gather all the notes that need to be duplicated into a temporary "manifest".
    // This avoids modifying the `building` dictionary while iterating over its keys.
    long manifest_capacity = 128;
    long manifest_count = 0;
    t_duplication_manifest_item *manifest = (t_duplication_manifest_item *)sysmem_newptr(manifest_capacity * sizeof(t_duplication_manifest_item));

    // 1. Identify all unique track numbers (the integer part of the track-offset key)
    long unique_track_num_count = 0;
    long *unique_track_nums = (long *)sysmem_newptr(num_keys * sizeof(long)); // Over-allocate
    for (long i = 0; i < num_keys; i++) {
        char *track_str, *bar_str, *prop_str;
        if (parse_hierarchical_key(keys[i], &track_str, &bar_str, &prop_str)) {
            long current_track_num;
            if (sscanf(track_str, "%ld-", &current_track_num) == 1) {
                int found = 0;
                for (long j = 0; j < unique_track_num_count; j++) {
                    if (unique_track_nums[j] == current_track_num) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    unique_track_nums[unique_track_num_count++] = current_track_num;
                }
            }
            sysmem_freeptr(track_str);
            sysmem_freeptr(bar_str);
            sysmem_freeptr(prop_str);
        }
    }

    // 2. For each unique track, find one representative span and add its notes to the manifest.
    for (long i = 0; i < unique_track_num_count; i++) {
        long track_num_to_process = unique_track_nums[i];
        t_symbol *source_track_sym = NULL;

        // Find the first track-offset symbol that matches the current track number.
        for (long j = 0; j < num_keys; j++) {
            char *track_str, *bar_str, *prop_str;
            if (parse_hierarchical_key(keys[j], &track_str, &bar_str, &prop_str)) {
                long current_key_track_num;
                if (sscanf(track_str, "%ld-", &current_key_track_num) == 1 && current_key_track_num == track_num_to_process) {
                    source_track_sym = gensym(track_str);
                    sysmem_freeptr(track_str);
                    sysmem_freeptr(bar_str);
                    sysmem_freeptr(prop_str);
                    break; // Found our representative
                }
                sysmem_freeptr(track_str);
                sysmem_freeptr(bar_str);
                sysmem_freeptr(prop_str);
            }
        }

        if (!source_track_sym) continue; // Should not happen, but a safe guard.

        // Now iterate through all keys again to find notes belonging to the source span
        for (long j = 0; j < num_keys; j++) {
            char *track_str, *bar_str, *prop_str;
            if (parse_hierarchical_key(keys[j], &track_str, &bar_str, &prop_str)) {
                if (strcmp(track_str, source_track_sym->s_name) == 0 && strcmp(prop_str, "absolutes") == 0) {
                    t_atom a;
                    dictionary_getatom(x->building, keys[j], &a);
                    t_atomarray *absolutes = (t_atomarray *)atom_getobj(&a);

                    t_symbol *scores_key = generate_hierarchical_key(source_track_sym, gensym(bar_str), gensym("scores"));
                    dictionary_getatom(x->building, scores_key, &a);
                    t_atomarray *scores = (t_atomarray *)atom_getobj(&a);

                    long abs_count; t_atom *abs_atoms;
                    atomarray_getatoms(absolutes, &abs_count, &abs_atoms);
                    long scores_count; t_atom *scores_atoms;
                    atomarray_getatoms(scores, &scores_count, &scores_atoms);

                    for (long k = 0; k < abs_count; k++) {
                        if (manifest_count >= manifest_capacity) {
                            manifest_capacity *= 2;
                            manifest = (t_duplication_manifest_item *)sysmem_resizeptr(manifest, manifest_capacity * sizeof(t_duplication_manifest_item));
                        }
                        manifest[manifest_count].track_number = track_num_to_process;
                        manifest[manifest_count].timestamp = atom_getfloat(abs_atoms + k);
                        manifest[manifest_count].score = atom_getfloat(scores_atoms + k);
                        manifest_count++;
                    }
                }
                sysmem_freeptr(track_str);
                sysmem_freeptr(bar_str);
                sysmem_freeptr(prop_str);
            }
        }
    }
    sysmem_freeptr(unique_track_nums);

    // --- PROCESSING PHASE ---
    buildspans_verbose_log(x, "Processing %ld notes from duplication manifest.", manifest_count);
    // Now that we have all the notes, we can safely add them to the building dictionary.
    if (manifest_count > 0) {
        qsort(manifest, manifest_count, sizeof(t_duplication_manifest_item), compare_manifest_items);
        long bar_length = buildspans_get_bar_length(x);
        if (bar_length > 0) {
            long original_track = x->current_track;

            for (long k = 0; k < manifest_count; k++) {
                x->current_track = manifest[k].track_number;
                buildspans_process_and_add_note(x, manifest[k].timestamp, manifest[k].score, new_offset, bar_length);
            }

            x->current_track = original_track;
        } else {
             object_warn((t_object *)x, "Bar length is not positive. Cannot process duplicated notes.");
        }
    }

    sysmem_freeptr(manifest);
    if (keys) sysmem_freeptr(keys);
    buildspans_visualize_memory(x);
}

// Handler for int messages on the 3rd inlet (proxy #2, track number)
void buildspans_track(t_buildspans *x, long n) {
    x->current_track = n;
    buildspans_verbose_log(x, "Track updated to: %ld", n);
}

// Handler for various messages, including palette symbol
void buildspans_anything(t_buildspans *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet_num = proxy_getinlet((t_object *)x);

    // Inlet 3 is the palette symbol inlet
    if (inlet_num == 3) {
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
    long bar_length = buildspans_get_bar_length(x);
    if (bar_length <= 0) {
        object_warn((t_object *)x, "Bar length is not positive. Ignoring input.");
        return;
    }
    if (argc != 2 || atom_gettype(argv) != A_FLOAT || atom_gettype(argv + 1) != A_FLOAT) {
        object_error((t_object *)x, "Input must be a list of two floats: timestamp and score.");
        return;
    }
    double timestamp = atom_getfloat(argv);
    double score = atom_getfloat(argv + 1);
    buildspans_verbose_log(x, "--- New Timestamp-Score Pair Received ---");
    buildspans_verbose_log(x, "Absolute timestamp: %.2f, Score: %.2f", timestamp, score);

    // 1. Find all unique track symbols for the current track.
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);

    long unique_tracks_count = 0;
    t_symbol **unique_track_syms = (t_symbol **)sysmem_newptr((num_keys + 1) * sizeof(t_symbol *));
    char track_prefix[32];
    snprintf(track_prefix, 32, "%ld-", x->current_track);

    if (keys) {
        for (long i = 0; i < num_keys; i++) {
            char *track_str, *bar_str, *prop_str;
            if (parse_hierarchical_key(keys[i], &track_str, &bar_str, &prop_str)) {
                if (strncmp(track_str, track_prefix, strlen(track_prefix)) == 0) {
                    t_symbol *current_track_sym = gensym(track_str);
                    int found = 0;
                    for (long j = 0; j < unique_tracks_count; j++) {
                        if (unique_track_syms[j] == current_track_sym) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        unique_track_syms[unique_tracks_count++] = current_track_sym;
                    }
                }
                sysmem_freeptr(track_str);
                sysmem_freeptr(bar_str);
                sysmem_freeptr(prop_str);
            }
        }
        sysmem_freeptr(keys);
    }

    // 2. Also consider the global current offset as a potential new span
    char current_track_str[64];
    snprintf(current_track_str, 64, "%ld-%ld", x->current_track, (long)round(x->current_offset));
    t_symbol *current_track_sym = gensym(current_track_str);
    int global_offset_found = 0;
    for (long i = 0; i < unique_tracks_count; i++) {
        if (unique_track_syms[i] == current_track_sym) {
            global_offset_found = 1;
            break;
        }
    }
    if (!global_offset_found) {
        unique_track_syms[unique_tracks_count++] = current_track_sym;
    }

    // 3. Add the note to each span (identified by its unique track symbol)
    buildspans_verbose_log(x, "Adding note to %ld span(s) on track %ld", unique_tracks_count, x->current_track);
    for (long i = 0; i < unique_tracks_count; i++) {
        const char *offset_part = strchr(unique_track_syms[i]->s_name, '-');
        if (offset_part) {
            long offset = atol(offset_part + 1);
            buildspans_process_and_add_note(x, timestamp, score, offset, bar_length);
        }
    }

    sysmem_freeptr(unique_track_syms);

    // --- Deferred Cleanup ---
    long num_ended_tracks;
    t_symbol **ended_track_keys;
    dictionary_getkeys(x->tracks_ended_in_current_event, &num_ended_tracks, &ended_track_keys);
    if (ended_track_keys) {
        for (long i = 0; i < num_ended_tracks; i++) {
            buildspans_cleanup_track_offset_if_needed(x, ended_track_keys[i]);
        }
        sysmem_freeptr(ended_track_keys);
    }
    dictionary_clear(x->tracks_ended_in_current_event);
}


void buildspans_process_and_add_note(t_buildspans *x, double timestamp, double score, long offset, long bar_length) {
    // Get current track symbol
    char track_str[64];
    snprintf(track_str, 64, "%ld-%ld", x->current_track, offset);
    t_symbol *track_sym = gensym(track_str);

    // Calculate bar timestamp
    double relative_timestamp = timestamp - offset;
    buildspans_verbose_log(x, "Relative timestamp (absolutes - offset): %.2f", relative_timestamp);
    long bar_timestamp_val = floor(relative_timestamp / bar_length) * bar_length;
    buildspans_verbose_log(x, "Calculated bar timestamp (rounded down to nearest %ld): %ld", bar_length, bar_timestamp_val);

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

        if (most_recent_bar_after_rating_check != -1 && bar_timestamp_val > most_recent_bar_after_rating_check + bar_length) {
            buildspans_verbose_log(x, "Discontiguous bar detected. New bar %ld is more than %ldms after last bar %ld.", bar_timestamp_val, bar_length, most_recent_bar_after_rating_check);
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
    dictionary_appendlong(x->building, offset_key, offset);
    buildspans_verbose_log(x, "%s %ld", offset_key->s_name, offset);
    t_atom offset_atom;
    atom_setlong(&offset_atom, offset);

    // Update palette
    t_symbol *palette_key = generate_hierarchical_key(track_sym, bar_sym, gensym("palette"));
    if (dictionary_hasentry(x->building, palette_key)) dictionary_deleteentry(x->building, palette_key);
    dictionary_appendsym(x->building, palette_key, x->current_palette);
    buildspans_verbose_log(x, "%s %s", palette_key->s_name, x->current_palette->s_name);
    t_atom palette_atom;
    atom_setsym(&palette_atom, x->current_palette);

    // Update absolutes array
    t_symbol *absolutes_key = generate_hierarchical_key(track_sym, bar_sym, gensym("absolutes"));
    t_atomarray *absolutes_array;
    if (!dictionary_hasentry(x->building, absolutes_key)) {
        absolutes_array = atomarray_new(0, NULL);
        buildspans_verbose_log(x, "LIST: Created absolutes_array: %p", absolutes_array);
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
    }

    // Update scores array
    t_symbol *scores_key = generate_hierarchical_key(track_sym, bar_sym, gensym("scores"));
    t_atomarray *scores_array;
    if (!dictionary_hasentry(x->building, scores_key)) {
        scores_array = atomarray_new(0, NULL);
        buildspans_verbose_log(x, "LIST: Created scores_array: %p", scores_array);
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
    buildspans_verbose_log(x, "LIST: Created new_span_array: %p", new_span_array);
    for (long i = 0; i < bar_timestamps_count; i++) {
        t_atom a; atom_setlong(&a, bar_timestamps[i]);
        atomarray_appendatom(new_span_array, &a);
    }
    char *span_str = atomarray_to_string(new_span_array);
    if (span_str) {
        for (long i = 0; i < bar_timestamps_count; i++) {
            char temp_bar_str[32]; snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);
            t_symbol *span_key = generate_hierarchical_key(track_sym, temp_bar_sym, gensym("span"));
            
            // Create a deep copy for each bar to ensure no shared ownership
            t_atomarray *span_copy = atomarray_deep_copy(new_span_array);
            t_atom span_copy_atom;
            atom_setobj(&span_copy_atom, (t_object *)span_copy);
            dictionary_appendatom(x->building, span_key, &span_copy_atom);

            // Logging
            char log_buffer[512];
            snprintf(log_buffer, 512, "%s %s", span_key->s_name, span_str);
            buildspans_verbose_log(x, "%s", log_buffer);
        }
        sysmem_freeptr(span_str);
    }
    object_free(new_span_array); // Free the original master array

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
        buildspans_verbose_log(x, "END: Created local span_to_output: %p", span_to_output);
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
        if (buildspans_validate_span_before_output(x, track_sym, span_to_output)) {
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

            // Outlet 3: Detailed span data
            buildspans_output_span_data(x, track_sym, span_to_output);

            // Outlet 2: Track number
            outlet_int(x->track_outlet, track_num_to_output);

            // Outlet 1: Span list
            outlet_anything(x->span_outlet, gensym("list"), (short)span_size, span_atoms);
        }

        if (local_span_created) {
            buildspans_verbose_log(x, "END: Freeing local span_to_output: %p", span_to_output);
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
                     // Log atomarrays before they are deleted
                     if (strcmp(key_prop, "absolutes") == 0 || strcmp(key_prop, "scores") == 0 || strcmp(key_prop, "span") == 0) {
                         t_atom a;
                         dictionary_getatom(x->building, keys[i], &a);
                         t_object *obj = atom_getobj(&a);
                         if (obj) {
                            buildspans_verbose_log(x, "END: Deleting %s object: %p", key_prop, obj);
                         }
                     }
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
    // Defer the cleanup check by adding the track to a temporary dictionary.
    dictionary_appendsym(x->tracks_ended_in_current_event, track_sym, 0);
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

    // --- Deferred Cleanup ---
    long num_ended_tracks;
    t_symbol **ended_track_keys;
    dictionary_getkeys(x->tracks_ended_in_current_event, &num_ended_tracks, &ended_track_keys);
    if (ended_track_keys) {
        for (long i = 0; i < num_ended_tracks; i++) {
            buildspans_cleanup_track_offset_if_needed(x, ended_track_keys[i]);
        }
        sysmem_freeptr(ended_track_keys);
    }
    dictionary_clear(x->tracks_ended_in_current_event);
}


void buildspans_assist(t_buildspans *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0:
                sprintf(s, "(list) Timestamp-Score Pair, (bang) Flush, (clear) Clear, (set_bar_buffer) Set Buffer Name");
                break;
            case 1:
                sprintf(s, "(float) Offset Timestamp");
                break;
            case 2:
                sprintf(s, "(int) Track Number");
                break;
            case 3:
                sprintf(s, "(symbol) Palette");
                break;
            case 4:
                sprintf(s, "(float) Local Bar Length");
                break;
        }
    } else { // ASSIST_OUTLET
        if (x->verbose) {
            switch (a) {
                case 0: sprintf(s, "Span Data (list)"); break;
                case 1: sprintf(s, "Track Number (int)"); break;
                case 2: sprintf(s, "Bar Data for Ended Spans (anything)"); break;
                case 3: sprintf(s, "Verbose Logging & Visualization Outlet"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "Span Data (list)"); break;
                case 1: sprintf(s, "Track Number (int)"); break;
                case 2: sprintf(s, "Bar Data for Ended Spans (anything)"); break;
            }
        }
    }
}

void buildspans_set_bar_buffer(t_buildspans *x, t_symbol *s) {
    if (s && s->s_name) {
        x->s_buffer_name = s;
        if (x->buffer_ref) {
            buffer_ref_set(x->buffer_ref, s);
        } else {
            x->buffer_ref = buffer_ref_new((t_object *)x, s);
        }
        buildspans_verbose_log(x, "Buffer set to: %s", s->s_name);
    } else {
        object_error((t_object *)x, "set_bar_buffer requires a valid buffer name.");
    }
}

void buildspans_local_bar_length(t_buildspans *x, double f) {
    x->local_bar_length = f;
    buildspans_verbose_log(x, "Local bar length set to: %.2f", f);
}

void buildspans_prune_span(t_buildspans *x, t_symbol *track_sym, long bar_to_keep) {
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return;

    // 1. Identify all bars to end.
    long end_count = 0;
    long *bars_to_end_vals = (long *)sysmem_newptr(num_keys * sizeof(long));

    for (long i = 0; i < num_keys; i++) {
        char *key_track, *key_bar, *key_prop;
        if (parse_hierarchical_key(keys[i], &key_track, &key_bar, &key_prop)) {
            if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_prop, "mean") == 0) {
                long bar_val = atol(key_bar);
                if (bar_val != bar_to_keep) {
                    bars_to_end_vals[end_count++] = bar_val;
                }
            }
            sysmem_freeptr(key_track);
            sysmem_freeptr(key_bar);
            sysmem_freeptr(key_prop);
        }
    }

    // 2. Finalize the state of the ended bars and then output.
    if (end_count > 0) {
        buildspans_verbose_log(x, "Pruning span for track %s, keeping bar %ld", track_sym->s_name, bar_to_keep);
        qsort(bars_to_end_vals, end_count, sizeof(long), compare_longs);

        // Create the list of ended bars
        t_atomarray *ended_span_array = atomarray_new(0, NULL);
        buildspans_verbose_log(x, "PRUNE: Created ended_span_array: %p", ended_span_array);
        for(long i = 0; i < end_count; ++i) {
            t_atom a; atom_setlong(&a, bars_to_end_vals[i]);
            atomarray_appendatom(ended_span_array, &a);
        }

        // Finalize the span data *before* outputting
        buildspans_verbose_log(x, "Finalizing ended span...");
        buildspans_finalize_and_log_span(x, track_sym, ended_span_array);

        // Validate and output the finalized data
        if (buildspans_validate_span_before_output(x, track_sym, ended_span_array)) {
            long span_size;
            t_atom *span_atoms;
            atomarray_getatoms(ended_span_array, &span_size, &span_atoms);

            long track_num_to_output;
            sscanf(track_sym->s_name, "%ld-", &track_num_to_output);

            // Outlet 3: Detailed span data
            buildspans_output_span_data(x, track_sym, ended_span_array);

            // Outlet 2: Track number
            outlet_int(x->track_outlet, track_num_to_output);

            // Outlet 1: Span list
            outlet_anything(x->span_outlet, gensym("list"), (short)span_size, span_atoms);
        }
        object_free(ended_span_array);
    }

    // 4. Delete keys for ended bars.
    t_symbol **keys_to_delete = (t_symbol**)sysmem_newptr(num_keys * sizeof(t_symbol*));
    long delete_count = 0;
    for (long i = 0; i < end_count; i++) {
        char bar_str[32];
        snprintf(bar_str, 32, "%ld", bars_to_end_vals[i]);
        for(long j=0; j<num_keys; ++j) {
            char *key_track, *key_bar, *key_prop;
            if (parse_hierarchical_key(keys[j], &key_track, &key_bar, &key_prop)) {
                if (strcmp(key_track, track_sym->s_name) == 0 && strcmp(key_bar, bar_str) == 0) {
                     // Log atomarrays before they are deleted
                     if (strcmp(key_prop, "absolutes") == 0 || strcmp(key_prop, "scores") == 0 || strcmp(key_prop, "span") == 0) {
                         t_atom a;
                         dictionary_getatom(x->building, keys[j], &a);
                         t_object *obj = atom_getobj(&a);
                         if (obj) {
                             buildspans_verbose_log(x, "PRUNE: Deleting %s object: %p", key_prop, obj);
                         }
                     }
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
    sysmem_freeptr(bars_to_end_vals);
    sysmem_freeptr(keys);

    char bar_to_keep_str[32];
    snprintf(bar_to_keep_str, 32, "%ld", bar_to_keep);
    buildspans_verbose_log(x, "Resetting kept bar...");
    buildspans_reset_bar_to_standalone(x, track_sym, gensym(bar_to_keep_str));

    // Since a span was ended, we need to schedule a cleanup check.
    if (end_count > 0) {
        dictionary_appendsym(x->tracks_ended_in_current_event, track_sym, 0);
    }

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
    }

    // Update span to be only itself
    t_atomarray *new_span_array = atomarray_new(0, NULL);
    buildspans_verbose_log(x, "RESET: Created new_span_array: %p", new_span_array);
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
    char *span_str = atomarray_to_string(span_array);

    for (long i = 0; i < span_size; i++) {
        long bar_ts = atom_getlong(&span_atoms[i]);
        char bar_str[32]; snprintf(bar_str, 32, "%ld", bar_ts);
        t_symbol *bar_sym = gensym(bar_str);

        t_symbol *rating_key = generate_hierarchical_key(track_sym, bar_sym, gensym("rating"));
        if(dictionary_hasentry(x->building, rating_key)) dictionary_deleteentry(x->building, rating_key);
        dictionary_appendatom(x->building, rating_key, &rating_atom);
        buildspans_verbose_log(x, "%s %.2f", rating_key->s_name, final_rating);

        // Create a deep copy for each bar to ensure no shared ownership
        t_atomarray *span_copy = atomarray_deep_copy(span_array);
        t_atom span_copy_atom;
        atom_setobj(&span_copy_atom, (t_object *)span_copy);
        t_symbol *span_key = generate_hierarchical_key(track_sym, bar_sym, gensym("span"));
        dictionary_appendatom(x->building, span_key, &span_copy_atom);

        if (span_str) {
            buildspans_verbose_log(x, "%s %s", span_key->s_name, span_str);
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

long find_next_offset(t_buildspans *x, long track_num_to_check, long offset_val_to_check) {
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return -1;

    t_dictionary *unique_offsets_dict = dictionary_new();
    char track_prefix[32];
    snprintf(track_prefix, 32, "%ld-", track_num_to_check);

    for (long i = 0; i < num_keys; i++) {
        char *track_str, *bar_str, *prop_str;
        if (parse_hierarchical_key(keys[i], &track_str, &bar_str, &prop_str)) {
            if (strncmp(track_str, track_prefix, strlen(track_prefix)) == 0) {
                const char *offset_part = strchr(track_str, '-');
                if (offset_part) {
                    long current_offset = atol(offset_part + 1);
                    char offset_key_str[32];
                    snprintf(offset_key_str, 32, "%ld", current_offset);
                    dictionary_appendlong(unique_offsets_dict, gensym(offset_key_str), 0);
                }
            }
            sysmem_freeptr(track_str);
            sysmem_freeptr(bar_str);
            sysmem_freeptr(prop_str);
        }
    }
    sysmem_freeptr(keys);

    long num_unique_offsets;
    t_symbol **offset_keys;
    dictionary_getkeys(unique_offsets_dict, &num_unique_offsets, &offset_keys);
    if (!offset_keys) {
        object_free(unique_offsets_dict);
        return -1;
    }

    long *offsets = (long *)sysmem_newptr(num_unique_offsets * sizeof(long));
    for(long i = 0; i < num_unique_offsets; i++) {
        offsets[i] = atol(offset_keys[i]->s_name);
    }
    
    qsort(offsets, num_unique_offsets, sizeof(long), compare_longs);
    
    long next_offset_time = -1;
    for (long i = 0; i < num_unique_offsets; i++) {
        if (offsets[i] > offset_val_to_check) {
            next_offset_time = offsets[i];
            break;
        }
    }

    sysmem_freeptr(offsets);
    sysmem_freeptr(offset_keys);
    object_free(unique_offsets_dict);

    return next_offset_time;
}

int buildspans_validate_span_before_output(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_to_output) {
    long track_num, offset_val;
    if (sscanf(track_sym->s_name, "%ld-%ld", &track_num, &offset_val) != 2) {
        return 0; // Should not happen
    }

    long next_offset = find_next_offset(x, track_num, offset_val);

    double earliest_absolute = -1.0;
    double latest_absolute = -1.0;

    long span_size;
    t_atom *span_atoms;
    atomarray_getatoms(span_to_output, &span_size, &span_atoms);

    for (long i = 0; i < span_size; i++) {
        long bar_ts = atom_getlong(&span_atoms[i]);
        char bar_str[32];
        snprintf(bar_str, 32, "%ld", bar_ts);
        t_symbol *bar_sym = gensym(bar_str);
        t_symbol *absolutes_key = generate_hierarchical_key(track_sym, bar_sym, gensym("absolutes"));

        if (dictionary_hasentry(x->building, absolutes_key)) {
            t_atom a;
            dictionary_getatom(x->building, absolutes_key, &a);
            t_atomarray *absolutes = (t_atomarray *)atom_getobj(&a);
            long ac;
            t_atom *av;
            atomarray_getatoms(absolutes, &ac, &av);

            for (long k = 0; k < ac; k++) {
                double current_time = atom_getfloat(av + k);
                if (earliest_absolute == -1.0 || current_time < earliest_absolute) {
                    earliest_absolute = current_time;
                }
                if (latest_absolute == -1.0 || current_time > latest_absolute) {
                    latest_absolute = current_time;
                }
            }
        }
    }

    if (earliest_absolute == -1.0) { // No notes in span
        buildspans_verbose_log(x, "Validation: Span %s has no notes. Aborting.", track_sym->s_name);
        return 0;
    }

    if (latest_absolute < offset_val) {
        buildspans_verbose_log(x, "Validation failed for %s: latest absolute (%.2f) is before its own offset (%ld). Aborting.", track_sym->s_name, latest_absolute, offset_val);
        return 0;
    }

    if (next_offset != -1 && earliest_absolute > next_offset) {
        buildspans_verbose_log(x, "Validation failed for %s: earliest absolute (%.2f) is after the next offset (%ld). Aborting.", track_sym->s_name, earliest_absolute, next_offset);
        return 0;
    }

    buildspans_verbose_log(x, "Validation successful for %s (earliest: %.2f, latest: %.2f, offset: %ld, next_offset: %ld)", track_sym->s_name, earliest_absolute, latest_absolute, offset_val, next_offset);
    return 1;
}

void buildspans_output_span_data(t_buildspans *x, t_symbol *track_sym, t_atomarray *span_atom_array) {
    if (!span_atom_array) return;

    long track_num_to_output;
    if (sscanf(track_sym->s_name, "%ld-", &track_num_to_output) != 1) {
        return; // Failed to parse track number
    }

    long span_size;
    t_atom *span_atoms;
    atomarray_getatoms(span_atom_array, &span_size, &span_atoms);

    const char* properties_to_output[] = {
        "absolutes", "scores", "mean", "offset", "palette", "rating", "span"
    };
    int num_properties = sizeof(properties_to_output) / sizeof(properties_to_output[0]);

    for (long i = 0; i < span_size; i++) {
        long bar_ts = atom_getlong(&span_atoms[i]);
        char bar_str[32];
        snprintf(bar_str, 32, "%ld", bar_ts);
        t_symbol *bar_sym = gensym(bar_str);

        for (int j = 0; j < num_properties; j++) {
            t_symbol *prop_sym = gensym(properties_to_output[j]);
            t_symbol *hierarchical_key = generate_hierarchical_key(track_sym, bar_sym, prop_sym);

            if (dictionary_hasentry(x->building, hierarchical_key)) {
                char output_key_str[256];
                snprintf(output_key_str, 256, "%ld::%s::%s", track_num_to_output, bar_sym->s_name, prop_sym->s_name);
                t_symbol *output_key_sym = gensym(output_key_str);

                t_atom a;
                dictionary_getatom(x->building, hierarchical_key, &a);
                short type = atom_gettype(&a);

                if (type == A_FLOAT) {
                    t_atom out_atom;
                    atom_setfloat(&out_atom, atom_getfloat(&a));
                    outlet_anything(x->log_outlet, output_key_sym, (short)1, &out_atom);
                } else if (type == A_LONG) {
                    t_atom out_atom;
                    atom_setlong(&out_atom, atom_getlong(&a));
                    outlet_anything(x->log_outlet, output_key_sym, (short)1, &out_atom);
                } else if (type == A_SYM) {
                    t_atom out_atom;
                    atom_setsym(&out_atom, atom_getsym(&a));
                    outlet_anything(x->log_outlet, output_key_sym, (short)1, &out_atom);
                } else if (type == A_OBJ) {
                    t_object *obj = atom_getobj(&a);
                    if (object_classname(obj) == gensym("atomarray")) {
                        t_atomarray *arr = (t_atomarray *)obj;
                        long ac;
                        t_atom *av;
                        atomarray_getatoms(arr, &ac, &av);
                        outlet_anything(x->log_outlet, output_key_sym, (short)ac, av);
                    }
                }
            }
        }
    }
}


void buildspans_cleanup_track_offset_if_needed(t_buildspans *x, t_symbol *track_offset_sym) {
    // 1. Parse the track and offset from the input symbol.
    long track_num_to_check, offset_val_to_check;
    if (sscanf(track_offset_sym->s_name, "%ld-%ld", &track_num_to_check, &offset_val_to_check) != 2) {
        return; // Not a valid track-offset symbol
    }
    buildspans_verbose_log(x, "--- Cleanup Check for %s ---", track_offset_sym->s_name);

    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(x->building, &num_keys, &keys);
    if (!keys) return;

    // --- Single Pass ---
    // In one loop, we will:
    // a) Find the oldest absolute time for the specific track-offset we are checking.
    // b) Collect all keys that belong to the track-offset to be checked.
    double oldest_absolute_time = -1.0;
    t_symbol **keys_to_delete = (t_symbol **)sysmem_newptr(num_keys * sizeof(t_symbol *));
    long delete_count = 0;
    
    for (long i = 0; i < num_keys; i++) {
        char *track_str, *bar_str, *prop_str;
        if (parse_hierarchical_key(keys[i], &track_str, &bar_str, &prop_str)) {
            // Check if it's the specific track-offset we are interested in
            if (strcmp(track_str, track_offset_sym->s_name) == 0) {
                // b) Collect this key for potential deletion
                keys_to_delete[delete_count++] = keys[i];

                // a) Find the oldest absolute time
                if (strcmp(prop_str, "absolutes") == 0) {
                    t_atom a;
                    dictionary_getatom(x->building, keys[i], &a);
                    t_atomarray *absolutes = (t_atomarray *)atom_getobj(&a);
                    long ac;
                    t_atom *av;
                    atomarray_getatoms(absolutes, &ac, &av);
                    for (long k = 0; k < ac; k++) {
                        double current_time = atom_getfloat(av + k);
                        if (oldest_absolute_time == -1.0 || current_time < oldest_absolute_time) {
                            oldest_absolute_time = current_time;
                        }
                    }
                }
            }

            sysmem_freeptr(track_str);
            sysmem_freeptr(bar_str);
            sysmem_freeptr(prop_str);
        }
    }

    // --- Process Collected Data ---
    
    // 2. Find the next chronological offset from our unique set.
    long next_offset_time = find_next_offset(x, track_num_to_check, offset_val_to_check);

    if (next_offset_time == -1) {
        buildspans_verbose_log(x, "Cleanup: No subsequent offset found for track %ld. No action taken.", track_num_to_check);
        goto cleanup;
    }
    buildspans_verbose_log(x, "Cleanup: Next offset for track %ld is %ld.", track_num_to_check, next_offset_time);

    if (oldest_absolute_time == -1.0) {
        buildspans_verbose_log(x, "Cleanup: No absolute timestamps found for %s. No action taken.", track_offset_sym->s_name);
        goto cleanup;
    }
    buildspans_verbose_log(x, "Cleanup: Oldest absolute time for %s is %.2f.", track_offset_sym->s_name, oldest_absolute_time);

    // 3. Compare and potentially delete.
    if (oldest_absolute_time >= next_offset_time) {
        buildspans_verbose_log(x, "Cleanup: Condition met (%.2f >= %ld). Deleting %ld keys for %s.", oldest_absolute_time, next_offset_time, delete_count, track_offset_sym->s_name);

        for (long i = 0; i < delete_count; i++) {
            dictionary_deleteentry(x->building, keys_to_delete[i]);
        }
        buildspans_visualize_memory(x);
    } else {
        buildspans_verbose_log(x, "Cleanup: Condition not met (%.2f < %ld). No action taken.", oldest_absolute_time, next_offset_time);
    }

cleanup:
    sysmem_freeptr(keys_to_delete);
    if(keys) sysmem_freeptr(keys);
}

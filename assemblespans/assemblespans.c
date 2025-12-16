#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_proto.h"
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
void assemblespans_flush_track(t_assemblespans *x, t_symbol *track_sym);

t_class *assemblespans_class;

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
    }
    return (x);
}

void assemblespans_free(t_assemblespans *x) {
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
    x->bar_length = 125; // Default bar length
    x->current_palette = gensym("");
    post("assemblespans cleared.");
}

// Handler for float messages on the 2nd inlet (proxy #1, offset)
void assemblespans_offset(t_assemblespans *x, double f) {
    char track_str[32];
    snprintf(track_str, 32, "%ld", x->current_track);
    t_symbol *track_sym = gensym(track_str);

    // Ensure a dictionary exists for the current track
    if (!dictionary_hasentry(x->working_memory, track_sym)) {
        t_dictionary *track_dict = dictionary_new();
        dictionary_appenddictionary(x->working_memory, track_sym, (t_object *)track_dict);
    }

    t_atom track_dict_atom;
    dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
    t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

    if (dictionary_hasentry(track_dict, gensym("offset"))) {
        dictionary_deleteentry(track_dict, gensym("offset"));
    }
    dictionary_appendfloat(track_dict, gensym("offset"), f);
    post("Offset for track %ld updated to: %.2f", x->current_track, f);
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
    double offset_val = 0.0;
    if (dictionary_hasentry(track_dict, gensym("offset"))) {
        dictionary_getfloat(track_dict, gensym("offset"), &offset_val);
    }
    post("Current offset for track %ld is: %.2f", x->current_track, offset_val);

    // Subtract offset and calculate bar timestamp
    double relative_timestamp = timestamp - offset_val;
    post("Relative timestamp (absolutes - offset): %.2f", relative_timestamp);

    long bar_timestamp_val = floor(relative_timestamp / x->bar_length) * x->bar_length;
    post("Calculated bar timestamp (rounded down to nearest %ld): %ld", x->bar_length, bar_timestamp_val);

    // --- Check for discontiguous bar ---
    long num_keys;
    t_symbol **keys;
    dictionary_getkeys(track_dict, &num_keys, &keys);
    if (keys && num_keys > 0) {
        long last_bar_timestamp = -1; // Initialize with a value lower than any possible timestamp

        // Find the most recent bar in the existing span
        for (long i = 0; i < num_keys; i++) {
            char *key_str = keys[i]->s_name;
            char *endptr;
            long val = strtol(key_str, &endptr, 10);
            if (*endptr == '\0') { // It's a numeric key
                if (last_bar_timestamp == -1 || val > last_bar_timestamp) {
                    last_bar_timestamp = val;
                }
            }
        }
        sysmem_freeptr(keys);

        // Check for discontinuity
        if (last_bar_timestamp != -1 && bar_timestamp_val > last_bar_timestamp + x->bar_length) {
            post("Discontiguous bar detected. New bar %ld is more than %ldms after last bar %ld.",
                 bar_timestamp_val, x->bar_length, last_bar_timestamp);
            assemblespans_flush_track(x, track_sym);
            // After flushing, the track_dict is invalid. We need to get it again.
            if (!dictionary_hasentry(x->working_memory, track_sym)) {
                track_dict = dictionary_new();
                dictionary_appenddictionary(x->working_memory, track_sym, (t_object *)track_dict);
            } else {
                 t_atom track_dict_atom;
                dictionary_getatom(x->working_memory, track_sym, &track_dict_atom);
                track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);
            }
        }
    }
    // --- End of check ---

    // Get bar dictionary (level 2)
    char bar_str[32];
    snprintf(bar_str, 32, "%ld", bar_timestamp_val);
    t_symbol *bar_sym = gensym(bar_str);

    t_dictionary *bar_dict;
    int new_bar_created = 0;
    if (!dictionary_hasentry(track_dict, bar_sym)) {
        bar_dict = dictionary_new();
        dictionary_appenddictionary(track_dict, bar_sym, (t_object *)bar_dict);
        post("Created new dictionary for: %s::%s", track_sym->s_name, bar_sym->s_name);
        new_bar_created = 1;
    } else {
        t_atom bar_dict_atom;
        dictionary_getatom(track_dict, bar_sym, &bar_dict_atom);
        bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);
    }

    // Store the current offset and palette in the bar dictionary
    if (dictionary_hasentry(bar_dict, gensym("offset"))) {
        dictionary_deleteentry(bar_dict, gensym("offset"));
    }
    dictionary_appendfloat(bar_dict, gensym("offset"), offset_val);
    if (dictionary_hasentry(bar_dict, gensym("palette"))) {
        dictionary_deleteentry(bar_dict, gensym("palette"));
    }
    dictionary_appendsym(bar_dict, gensym("palette"), x->current_palette);
    post("Updated dictionary entry: %s::%s::offset -> %.2f", track_sym->s_name, bar_sym->s_name, offset_val);
    post("Updated dictionary entry: %s::%s::palette -> %s", track_sym->s_name, bar_sym->s_name, x->current_palette->s_name);


    // Get or create the 'absolutes' atomarray (level 3)
    t_atomarray *absolutes_array;
    if (!dictionary_hasentry(bar_dict, gensym("absolutes"))) {
        absolutes_array = atomarray_new(0, NULL);
        t_atom absolutes_atom;
        atom_setobj(&absolutes_atom, (t_object *)absolutes_array);
        dictionary_appendatom(bar_dict, gensym("absolutes"), &absolutes_atom);
    } else {
        t_atom absolutes_atom;
        dictionary_getatom(bar_dict, gensym("absolutes"), &absolutes_atom);
        absolutes_array = (t_atomarray *)atom_getobj(&absolutes_atom);
    }

    // Append the new absolute timestamp
    t_atom new_absolute_atom;
    atom_setfloat(&new_absolute_atom, timestamp);
    atomarray_appendatom(absolutes_array, &new_absolute_atom);

    // Post the entire absolutes array
    long str_size_abs = 256;
    char *abs_str = (char *)sysmem_newptr(str_size_abs);
    long offset_abs = snprintf(abs_str, str_size_abs, "Updated %s::%s::absolutes: [ ", track_sym->s_name, bar_sym->s_name);
    for (long j = 0; j < atomarray_getsize(absolutes_array); j++) {
        char temp[32];
        t_atom item;
        atomarray_getindex(absolutes_array, j, &item);
        int len = snprintf(temp, 32, "%.2f ", atom_getfloat(&item));
        if (offset_abs + len + 2 >= str_size_abs) { // +2 for " ]"
            str_size_abs *= 2;
            abs_str = (char *)sysmem_resizeptr(abs_str, str_size_abs);
        }
        strcat(abs_str, temp);
        offset_abs += len;
    }
    strcat(abs_str, "]");
    post(abs_str);
    sysmem_freeptr(abs_str);


    // Get or create the 'scores' atomarray (level 3)
    t_atomarray *scores_array;
    if (!dictionary_hasentry(bar_dict, gensym("scores"))) {
        scores_array = atomarray_new(0, NULL);
        t_atom scores_atom;
        atom_setobj(&scores_atom, (t_object *)scores_array);
        dictionary_appendatom(bar_dict, gensym("scores"), &scores_atom);
    } else {
        t_atom scores_atom;
        dictionary_getatom(bar_dict, gensym("scores"), &scores_atom);
        scores_array = (t_atomarray *)atom_getobj(&scores_atom);
    }

    // Append the new score
    t_atom new_score_atom;
    atom_setfloat(&new_score_atom, score);
    atomarray_appendatom(scores_array, &new_score_atom);

    // Post the entire scores array
    long str_size_scores = 256;
    char *scores_str = (char *)sysmem_newptr(str_size_scores);
    long offset_scores = snprintf(scores_str, str_size_scores, "Updated %s::%s::scores: [ ", track_sym->s_name, bar_sym->s_name);
    for (long j = 0; j < atomarray_getsize(scores_array); j++) {
        char temp[32];
        t_atom item;
        atomarray_getindex(scores_array, j, &item);
        int len = snprintf(temp, 32, "%.2f ", atom_getfloat(&item));
        if (offset_scores + len + 2 >= str_size_scores) { // +2 for " ]"
            str_size_scores *= 2;
            scores_str = (char *)sysmem_resizeptr(scores_str, str_size_scores);
        }
        strcat(scores_str, temp);
        offset_scores += len;
    }
    strcat(scores_str, "]");
    post(scores_str);
    sysmem_freeptr(scores_str);

    // Calculate and store the mean of the scores
    long scores_count = atomarray_getsize(scores_array);
    if (scores_count > 0) {
        double sum = 0.0;
        for (long i = 0; i < scores_count; i++) {
            t_atom score_atom;
            atomarray_getindex(scores_array, i, &score_atom);
            sum += atom_getfloat(&score_atom);
        }
        double mean = sum / scores_count;
        if (dictionary_hasentry(bar_dict, gensym("mean"))) {
            dictionary_deleteentry(bar_dict, gensym("mean"));
        }
        dictionary_appendfloat(bar_dict, gensym("mean"), mean);
        post("Updated dictionary entry: %s::%s::mean -> %.2f", track_sym->s_name, bar_sym->s_name, mean);
    }

    if (new_bar_created) {
        // Collect all numeric bar timestamps for the current track
        long num_keys;
        t_symbol **keys;
        dictionary_getkeys(track_dict, &num_keys, &keys);

        long bar_timestamps_count = 0;
        long *bar_timestamps = (long *)sysmem_newptr(num_keys * sizeof(long));

        for (long i = 0; i < num_keys; i++) {
            char *key_str = keys[i]->s_name;
            char *endptr;
            long val = strtol(key_str, &endptr, 10);

            // Check if the entire string was consumed, excluding a leading minus sign if present
            if (*endptr == '\0' && (isdigit(key_str[0]) || (key_str[0] == '-' && key_str[1] != '\0'))) {
                 bar_timestamps[bar_timestamps_count++] = val;
            }
        }

        // Sort the bar timestamps
        qsort(bar_timestamps, bar_timestamps_count, sizeof(long), compare_longs);

        // Create the definitive span atomarray
        t_atomarray *span_array = atomarray_new(0, NULL);
        for (long i = 0; i < bar_timestamps_count; i++) {
            t_atom bar_atom;
            atom_setlong(&bar_atom, bar_timestamps[i]);
            atomarray_appendatom(span_array, &bar_atom);
        }

        // Back-propagate the updated span to all bars in the track
        for (long i = 0; i < bar_timestamps_count; i++) {
            char temp_bar_str[32];
            snprintf(temp_bar_str, 32, "%ld", bar_timestamps[i]);
            t_symbol *temp_bar_sym = gensym(temp_bar_str);

            t_atom bar_dict_atom;
            dictionary_getatom(track_dict, temp_bar_sym, &bar_dict_atom);
            t_dictionary *temp_bar_dict = (t_dictionary *)atom_getobj(&bar_dict_atom);

            t_atom span_atom;
            atom_setobj(&span_atom, (t_object *)span_array);

            if (dictionary_hasentry(temp_bar_dict, gensym("span"))) {
                dictionary_deleteentry(temp_bar_dict, gensym("span"));
            }
            dictionary_appendatom(temp_bar_dict, gensym("span"), &span_atom);
        }

        // Post the entire span array for diagnostics
        long str_size_span = 256;
        char *span_str = (char *)sysmem_newptr(str_size_span);
        long offset_span = snprintf(span_str, str_size_span, "Updated %s::span: [ ", track_sym->s_name);
        for (long j = 0; j < atomarray_getsize(span_array); j++) {
            char temp[32];
            t_atom item;
            atomarray_getindex(span_array, j, &item);
            int len = snprintf(temp, 32, "%ld ", atom_getlong(&item));
            if (offset_span + len + 2 >= str_size_span) { // +2 for " ]"
                str_size_span *= 2;
                span_str = (char *)sysmem_resizeptr(span_str, str_size_span);
            }
            strcat(span_str, temp);
            offset_span += len;
        }
        strcat(span_str, "]");
        post(span_str);
        sysmem_freeptr(span_str);

        // Free memory
        sysmem_freeptr(bar_timestamps);
        if (keys) sysmem_freeptr(keys);
        // Do not free span_array here, it's owned by the dictionaries now
    }
}

void assemblespans_flush_track(t_assemblespans *x, t_symbol *track_sym) {
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
        post("Flushing span for track %s...", track_sym->s_name);
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
        assemblespans_flush_track(x, tracks[i]);
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
                sprintf(s, "(list) Timestamp-Score Pair, (clear) Clear Memory");
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

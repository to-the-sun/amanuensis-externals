#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_proto.h"
#include <math.h>

typedef struct _assemblespans {
    t_object s_obj;
    t_dictionary *working_memory;
    long current_track;
    long bar_length;
    t_symbol *current_palette;
} t_assemblespans;

// Function prototypes
void *assemblespans_new(void);
void assemblespans_free(t_assemblespans *x);
void assemblespans_list(t_assemblespans *x, t_symbol *s, long argc, t_atom *argv);
void assemblespans_int(t_assemblespans *x, long n);
void assemblespans_offset(t_assemblespans *x, double f);
void assemblespans_bar_length(t_assemblespans *x, long n);
void assemblespans_track(t_assemblespans *x, long n);
void assemblespans_palette(t_assemblespans *x, t_symbol *s);
void assemblespans_assist(t_assemblespans *x, void *b, long m, long a, char *s);
void post_working_memory(t_assemblespans *x);

t_class *assemblespans_class;

void ext_main(void *r) {
    t_class *c;
    c = class_new("assemblespans", (method)assemblespans_new, (method)assemblespans_free, (short)sizeof(t_assemblespans), 0L, A_GIMME, 0);
    class_addmethod(c, (method)assemblespans_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)assemblespans_int, "int", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_track, "in2", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_bar_length, "in3", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_palette, "symbol", A_DEFSYM, 0);
    class_addmethod(c, (method)assemblespans_assist, "assist", A_CANT, 0);
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
    }
    return (x);
}

void assemblespans_free(t_assemblespans *x) {
    if (x->working_memory) {
        object_free(x->working_memory);
    }
}

// Handler for float messages on inlet 2 (offset)
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

    dictionary_appendfloat(track_dict, gensym("offset"), f);
    post("Offset for track %ld updated to: %.2f", x->current_track, f);
}

// Handler for int messages on inlet 4 (bar length)
void assemblespans_bar_length(t_assemblespans *x, long n) {
    x->bar_length = n;
    post("Bar length updated to: %ld", n);
}

// Handler for int messages on inlet 3 (track number)
void assemblespans_track(t_assemblespans *x, long n) {
    x->current_track = n;
    post("Track updated to: %ld", n);
}

// Handler for symbol messages on inlet 5 (palette)
void assemblespans_palette(t_assemblespans *x, t_symbol *s) {
    long inlet_num = proxy_getinlet((t_object *)x);
    if (inlet_num == 4) {
        x->current_palette = s;
        post("Palette set to: %s", s->s_name);
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

    // Get bar dictionary (level 2)
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

    // Store the current offset and palette in the bar dictionary
    dictionary_appendfloat(bar_dict, gensym("offset"), offset_val);
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
    post("Appended to dictionary entry: %s::%s::absolutes -> %.2f", track_sym->s_name, bar_sym->s_name, timestamp);

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
    post("Appended to dictionary entry: %s::%s::scores -> %.2f", track_sym->s_name, bar_sym->s_name, score);

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
        dictionary_appendfloat(bar_dict, gensym("mean"), mean);
        post("Updated dictionary entry: %s::%s::mean -> %.2f", track_sym->s_name, bar_sym->s_name, mean);
    }

    post_working_memory(x);
}


// Handler for int messages on the main inlet
void assemblespans_int(t_assemblespans *x, long n) {
    object_error((t_object *)x, "Invalid input: main inlet expects a list of two floats.");
}

void post_working_memory(t_assemblespans *x) {
    long num_tracks;
    t_symbol **tracks = NULL;
    dictionary_getkeys(x->working_memory, &num_tracks, &tracks);
    post("--- Working Memory ---");
    for (int i = 0; i < num_tracks; i++) {
        t_atom track_dict_atom;
        dictionary_getatom(x->working_memory, tracks[i], &track_dict_atom);
        t_dictionary *track_dict = (t_dictionary *)atom_getobj(&track_dict_atom);

        post("Track %s:", tracks[i]->s_name);
        if (dictionary_hasentry(track_dict, gensym("notes"))) {
            t_atom notes_atom;
            dictionary_getatom(track_dict, gensym("notes"), &notes_atom);
            t_atomarray *notes = (t_atomarray *)atom_getobj(&notes_atom);

            long str_size = 1024;
            char *notes_str = (char *)sysmem_newptr(str_size);
            long offset = snprintf(notes_str, str_size, "  Notes: ");

            for (long j = 0; j < atomarray_getsize(notes); j++) {
                char temp[32];
                t_atom note;
                atomarray_getindex(notes, j, &note);
                int len = snprintf(temp, 32, "%.2f ", atom_getfloat(&note));
                if (offset + len >= str_size) {
                    str_size *= 2;
                    notes_str = (char *)sysmem_resizeptr(notes_str, str_size);
                }
                strcat(notes_str, temp);
                offset += len;
            }
            post(notes_str);
            sysmem_freeptr(notes_str);
        }
        if (dictionary_hasentry(track_dict, gensym("offset"))) {
            double offset_val;
            dictionary_getfloat(track_dict, gensym("offset"), &offset_val);
            post("  Offset: %.2f", offset_val);
        }
    }
    if (tracks) {
        sysmem_freeptr(tracks);
    }
    post("--------------------");
}

void assemblespans_assist(t_assemblespans *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0:
                sprintf(s, "(list) Timestamp-Score Pair");
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

#include "ext.h"
#include <math.h>
#include "ext_obex.h"
#include "ext_dictobj.h"

typedef struct _assemblespans {
    t_object s_obj;
    t_dictionary *working_memory;
    long current_track;
    double bar;
} t_assemblespans;

// Function prototypes
void *assemblespans_new(void);
void assemblespans_free(t_assemblespans *x);
void assemblespans_float(t_assemblespans *x, double f);
void assemblespans_int(t_assemblespans *x, long n);
void assemblespans_offset(t_assemblespans *x, double f);
void assemblespans_bar(t_assemblespans *x, double f);
void assemblespans_track(t_assemblespans *x, long n);
void assemblespans_assist(t_assemblespans *x, void *b, long m, long a, char *s);
void post_working_memory(t_assemblespans *x);

t_class *assemblespans_class;

void ext_main(void *r) {
    t_class *c;
    c = class_new("assemblespans", (method)assemblespans_new, (method)assemblespans_free, (short)sizeof(t_assemblespans), 0L, 0);
    class_addmethod(c, (method)assemblespans_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_int, "int", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_offset, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_bar, "ft2", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_track, "in3", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_assist, "assist", A_CANT, 0);
    class_register(CLASS_BOX, c);
    assemblespans_class = c;
}

void *assemblespans_new(void) {
    t_assemblespans *x = (t_assemblespans *)object_alloc(assemblespans_class);
    if (x) {
        x->working_memory = dictionary_new();
        x->current_track = 0;
        x->bar = 125.0; // Default bar length
        // Inlets are created from right to left.
        intin((t_object *)x, 3);    // Track Number
        floatin((t_object *)x, 2);  // Bar Length
        floatin((t_object *)x, 1);  // Offset
    }
    return (x);
}

void assemblespans_free(t_assemblespans *x) {
    if (x->working_memory) {
        object_free(x->working_memory);
    }
}

// Handler for float messages on inlet 1 (offset)
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

// Handler for float messages on inlet 2 (bar length)
void assemblespans_bar(t_assemblespans *x, double f) {
    x->bar = f;
    post("Bar length updated to: %.2f", f);
}

// Handler for int messages on inlet 3 (track number)
void assemblespans_track(t_assemblespans *x, long n) {
    x->current_track = n;
    post("Track updated to: %ld", n);
}

// Handler for float messages on the main inlet (note timestamp)
void assemblespans_float(t_assemblespans *x, double f) {
    post("--- New Timestamp Received ---");
    post("Absolute timestamp: %.2f", f);

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
    double relative_timestamp = f - offset_val;
    post("Relative timestamp (absolutes - offset): %.2f", relative_timestamp);

    long bar_timestamp_val = floor(relative_timestamp / x->bar) * x->bar;
    post("Calculated bar timestamp (rounded down to nearest %.2f): %ld", x->bar, bar_timestamp_val);

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

    // Store the current offset in the bar dictionary
    dictionary_appendfloat(bar_dict, gensym("offset"), offset_val);
    post("Updated dictionary entry: %s::%s::offset -> %.2f", track_sym->s_name, bar_sym->s_name, offset_val);

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
    atom_setfloat(&new_absolute_atom, f);
    atomarray_appendatom(absolutes_array, &new_absolute_atom);
    post("Appended to dictionary entry: %s::%s::absolutes -> %.2f", track_sym->s_name, bar_sym->s_name, f);

    post_working_memory(x);
}

// Handler for int messages on the main inlet
void assemblespans_int(t_assemblespans *x, long n) {
    assemblespans_float(x, (double)n);
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
                sprintf(s, "(float) Note Timestamp");
                break;
            case 1:
                sprintf(s, "(float) Offset Timestamp");
                break;
            case 2:
                sprintf(s, "(float) Bar Length");
                break;
            case 3:
                sprintf(s, "(int) Track Number");
                break;
        }
    }
}

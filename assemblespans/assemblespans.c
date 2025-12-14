#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"

typedef struct _assemblespans {
    t_object s_obj;
    t_dictionary *working_memory;
    long current_track;
} t_assemblespans;

// Function prototypes
void *assemblespans_new(void);
void assemblespans_free(t_assemblespans *x);
void assemblespans_float(t_assemblespans *x, double f);
void assemblespans_int(t_assemblespans *x, long n);
void assemblespans_assist(t_assemblespans *x, void *b, long m, long a, char *s);
void post_working_memory(t_assemblespans *x);

t_class *assemblespans_class;

void ext_main(void *r) {
    t_class *c;
    c = class_new("assemblespans", (method)assemblespans_new, (method)assemblespans_free, (short)sizeof(t_assemblespans), 0L, 0);
    class_addmethod(c, (method)assemblespans_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_int, "int", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_float, "ft1", A_FLOAT, 0);
    class_addmethod(c, (method)assemblespans_int, "in2", A_LONG, 0);
    class_addmethod(c, (method)assemblespans_assist, "assist", A_CANT, 0);
    class_register(CLASS_BOX, c);
    assemblespans_class = c;
}

void *assemblespans_new(void) {
    t_assemblespans *x = (t_assemblespans *)object_alloc(assemblespans_class);
    if (x) {
        x->working_memory = dictionary_new();
        x->current_track = 0;
        // Inlets are created from right to left.
        // The last one created is the leftmost proxy inlet.
        intin((t_object *)x, 2);    // Right-most inlet, index 2
        floatin((t_object *)x, 1);  // Middle inlet, index 1
    }
    return (x);
}

void assemblespans_free(t_assemblespans *x) {
    if (x->working_memory) {
        object_free(x->working_memory);
    }
}

void assemblespans_float(t_assemblespans *x, double f) {
    long inlet = proxy_getinlet((t_object *)x);

    // Max number boxes can send floats to int inlets, so we handle track selection here too.
    if (inlet == 2) {
        x->current_track = (long)f;
        post("Current track set to: %ld", x->current_track);
        return;
    }

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

    if (inlet == 0) { // Note inlet
        t_atomarray *notes;
        t_atom notes_atom;
        if (!dictionary_hasentry(track_dict, gensym("notes"))) {
            notes = atomarray_new(0, NULL);
            atom_setobj(&notes_atom, (t_object *)notes);
            dictionary_appendatom(track_dict, gensym("notes"), &notes_atom);
        } else {
            dictionary_getatom(track_dict, gensym("notes"), &notes_atom);
            notes = (t_atomarray *)atom_getobj(&notes_atom);
        }
        t_atom note_atom;
        atom_setfloat(&note_atom, f);
        atomarray_appendatom(notes, &note_atom);
    } else if (inlet == 1) { // Offset inlet
        dictionary_appendfloat(track_dict, gensym("offset"), f);
    }

    post_working_memory(x);
}

void assemblespans_int(t_assemblespans *x, long n) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 2) {
        x->current_track = n;
        post("Current track set to: %ld", n);
    } else {
        // Silently ignore integers on other inlets, or post a message if that's preferred.
        // post("Integer received on unexpected inlet: %ld", inlet);
    }
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
                sprintf(s, "(int) Track Number");
                break;
        }
    }
}

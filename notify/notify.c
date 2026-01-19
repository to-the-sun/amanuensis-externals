#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_atomarray.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct _note {
    double absolute;
    double score;
    double offset;
    t_symbol *track;
    t_symbol *palette;
} t_note;

typedef struct _notify {
    t_object s_obj;
    t_symbol *dict_name;
    long verbose;
    void *out_abs_score; // Outlet 1 (Leftmost)
    void *out_offset;    // Outlet 2
    void *out_track;     // Outlet 3
    void *out_palette;   // Outlet 4
    void *out_verbose;   // Outlet 5 (Rightmost, optional)
} t_notify;

t_class *notify_class;

// Function prototypes
void *notify_new(t_symbol *s, long argc, t_atom *argv);
void notify_free(t_notify *x);
void notify_bang(t_notify *x);
void notify_assist(t_notify *x, void *b, long m, long a, char *s);
int note_compare(const void *a, const void *b);
void notify_verbose_log(t_notify *x, const char *fmt, ...);

void ext_main(void *r) {
    t_class *c;
    c = class_new("notify", (method)notify_new, (method)notify_free, (short)sizeof(t_notify), 0L, A_GIMME, 0);
    class_addmethod(c, (method)notify_bang, "bang", 0);
    class_addmethod(c, (method)notify_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_notify, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");

    class_register(CLASS_BOX, c);
    notify_class = c;
}

void notify_verbose_log(t_notify *x, const char *fmt, ...) {
    if (x->verbose && x->out_verbose) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        outlet_anything(x->out_verbose, gensym(buf), 0, NULL);
    }
}

void *notify_new(t_symbol *s, long argc, t_atom *argv) {
    t_notify *x = (t_notify *)object_alloc(notify_class);
    if (x) {
        x->dict_name = gensym("");
        x->verbose = 0;
        x->out_verbose = NULL;

        // Attributes need to be processed before outlet creation if they affect it
        attr_args_process(x, argc, argv);

        // Max SDK outlet_new prepends outlets (right-to-left).
        // To have Outlet 1 on the left and Outlet 4 on the right, create them in reverse.

        // Rightmost is Verbose (if enabled)
        if (x->verbose) {
            x->out_verbose = outlet_new((t_object *)x, NULL);
        }

        // Next to the left: Palette (Outlet 4)
        x->out_palette = outlet_new((t_object *)x, NULL);

        // Next to the left: Track (Outlet 3)
        x->out_track = outlet_new((t_object *)x, NULL);

        // Next to the left: Offset (Outlet 2)
        x->out_offset = outlet_new((t_object *)x, NULL);

        // Leftmost: Abs/Score (Outlet 1)
        x->out_abs_score = outlet_new((t_object *)x, NULL);

        // Re-process dict_name if it was a positional argument
        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->dict_name = atom_getsym(argv);
        }
    }
    return (x);
}

void notify_free(t_notify *x) {
    // Nothing special to free
}

int note_compare(const void *a, const void *b) {
    t_note *noteA = (t_note *)a;
    t_note *noteB = (t_note *)b;
    if (noteA->absolute < noteB->absolute) return -1;
    if (noteA->absolute > noteB->absolute) return 1;
    return 0;
}

void notify_bang(t_notify *x) {
    t_dictionary *dict = dictobj_findregistered_retain(x->dict_name);
    if (!dict) {
        object_error((t_object *)x, "could not find dictionary named %s", x->dict_name->s_name);
        return;
    }

    long num_tracks = 0;
    t_symbol **track_keys = NULL;
    dictionary_getkeys(dict, &num_tracks, &track_keys);

    t_note *all_notes = NULL;
    long total_notes = 0;
    long notes_capacity = 100;
    all_notes = (t_note *)sysmem_newptr(sizeof(t_note) * notes_capacity);

    for (long i = 0; i < num_tracks; i++) {
        t_symbol *track_sym = track_keys[i];

        t_dictionary *track_dict = NULL;
        dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict);
        if (!track_dict) continue;

        long num_bars = 0;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        for (long j = 0; j < num_bars; j++) {
            t_symbol *bar_sym = bar_keys[j];

            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
            if (!bar_dict) continue;

            t_atom absolute_atom, score_atom, offset_atom, palette_atom;
            t_atomarray *absolutes_aa = NULL;

            double offset = 0;
            t_atomarray *offset_aa = NULL;
            if (dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_aa) == MAX_ERR_NONE && offset_aa) {
                t_atom o_atom;
                if (atomarray_getindex(offset_aa, 0, &o_atom) == MAX_ERR_NONE) {
                    offset = atom_getfloat(&o_atom);
                }
            } else if (dictionary_getatom(bar_dict, gensym("offset"), &offset_atom) == MAX_ERR_NONE) {
                offset = atom_getfloat(&offset_atom);
            }

            t_symbol *palette = gensym("");
            t_atomarray *palette_aa = NULL;
            if (dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_aa) == MAX_ERR_NONE && palette_aa) {
                t_atom p_atom;
                if (atomarray_getindex(palette_aa, 0, &p_atom) == MAX_ERR_NONE) {
                    palette = atom_getsym(&p_atom);
                }
            } else if (dictionary_getatom(bar_dict, gensym("palette"), &palette_atom) == MAX_ERR_NONE) {
                palette = atom_getsym(&palette_atom);
            }

            // Get absolutes and scores
            long num_entries = 0;
            if (dictionary_getatomarray(bar_dict, gensym("absolutes"), (t_object **)&absolutes_aa) == MAX_ERR_NONE && absolutes_aa) {
                long aa_len = 0;
                t_atom *aa_atoms = NULL;
                atomarray_getatoms(absolutes_aa, &aa_len, &aa_atoms);

                t_atomarray *scores_aa_check = NULL;
                if (dictionary_getatomarray(bar_dict, gensym("scores"), (t_object **)&scores_aa_check) == MAX_ERR_NONE && scores_aa_check) {
                    long scores_len = 0;
                    t_atom *scores_atoms = NULL;
                    atomarray_getatoms(scores_aa_check, &scores_len, &scores_atoms);

                    num_entries = aa_len < scores_len ? aa_len : scores_len; // Safety

                    for (long k = 0; k < num_entries; k++) {
                        if (total_notes >= notes_capacity) {
                            notes_capacity *= 2;
                            all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                        }
                        all_notes[total_notes].absolute = atom_getfloat(&aa_atoms[k]);
                        all_notes[total_notes].score = atom_getfloat(&scores_atoms[k]);
                        all_notes[total_notes].offset = offset;
                        all_notes[total_notes].track = track_sym;
                        all_notes[total_notes].palette = palette;
                        total_notes++;
                    }
                }
            } else if (dictionary_getatom(bar_dict, gensym("absolutes"), &absolute_atom) == MAX_ERR_NONE) {
                // Scalar case
                if (total_notes >= notes_capacity) {
                    notes_capacity *= 2;
                    all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                }
                all_notes[total_notes].absolute = atom_getfloat(&absolute_atom);

                if (dictionary_getatom(bar_dict, gensym("scores"), &score_atom) == MAX_ERR_NONE) {
                    all_notes[total_notes].score = atom_getfloat(&score_atom);
                } else {
                    all_notes[total_notes].score = 0;
                }

                all_notes[total_notes].offset = offset;
                all_notes[total_notes].track = track_sym;
                all_notes[total_notes].palette = palette;
                total_notes++;
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }
    if (track_keys) sysmem_freeptr(track_keys);

    // Sort notes chronologically
    if (total_notes > 1) {
        qsort(all_notes, total_notes, sizeof(t_note), note_compare);
    }

    // Report ordered notes
    for (long i = 0; i < total_notes; i++) {
        notify_verbose_log(x, "Ordered Note %ld: abs=%.2f, score=%.2f, offset=%.2f, track=%s, palette=%s",
                           i, all_notes[i].absolute, all_notes[i].score, all_notes[i].offset,
                           all_notes[i].track ? all_notes[i].track->s_name : "NULL",
                           all_notes[i].palette ? all_notes[i].palette->s_name : "NULL");
    }

    // Clear dictionary
    dictionary_clear(dict);
    notify_verbose_log(x, "Dictionary cleared.");

    // Output notes
    for (long i = 0; i < total_notes; i++) {
        // Right-to-left firing order (rightmost outlet first)

        // Outlet 4: palette (Rightmost non-verbose)
        outlet_anything(x->out_palette, all_notes[i].palette, 0, NULL);

        // Outlet 3: track
        if (all_notes[i].track == NULL) {
            outlet_int(x->out_track, 0);
        } else if (all_notes[i].track == gensym("")) {
            outlet_int(x->out_track, 0);
        } else {
            long track_val = atol(all_notes[i].track->s_name);
            outlet_int(x->out_track, track_val);
        }

        // Outlet 2: offset
        outlet_float(x->out_offset, all_notes[i].offset);

        // Outlet 1: [absolute, score] (Leftmost)
        t_atom list_atoms[2];
        atom_setfloat(&list_atoms[0], all_notes[i].absolute);
        atom_setfloat(&list_atoms[1], all_notes[i].score);
        outlet_list(x->out_abs_score, NULL, 2, list_atoms);
    }

    if (all_notes) sysmem_freeptr(all_notes);
    object_release((t_object *)dict);
}

void notify_assist(t_notify *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "bang to dump dictionary");
    } else {
        // Outlet indices are 0-indexed from LEFT to RIGHT
        switch (a) {
            case 0: sprintf(s, "Abs Timestamp and Score (list)"); break;
            case 1: sprintf(s, "Offset (float)"); break;
            case 2: sprintf(s, "Track (int)"); break;
            case 3: sprintf(s, "Palette (symbol)"); break;
            case 4: if (x->verbose) sprintf(s, "Verbose Logging"); break;
        }
    }
}

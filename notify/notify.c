#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_atomarray.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

typedef struct _note {
    double absolute;
    double score;
    double offset;
    double bar_ts;
    t_symbol *track;
    t_symbol *palette;
} t_note;

typedef struct _notify {
    t_object s_obj;
    t_symbol *dict_name;
    long verbose;
    void *out_abs_score; // Outlet 1 (Leftmost, Index 0)
    void *out_offset;    // Outlet 2 (Index 1)
    void *out_track;     // Outlet 3 (Index 2)
    void *out_palette;   // Outlet 4 (Index 3)
    void *out_descript;  // Outlet 5 (Index 4)
    void *out_verbose;   // Outlet 6 (Rightmost, optional, Index 5)
    t_buffer_ref *buffer_ref;
    double local_bar_length;
    long instance_id;
} t_notify;

t_class *notify_class;

// Function prototypes
void *notify_new(t_symbol *s, long argc, t_atom *argv);
void notify_free(t_notify *x);
void notify_bang(t_notify *x);
void notify_fill(t_notify *x);
t_max_err notify_notify(t_notify *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void notify_assist(t_notify *x, void *b, long m, long a, char *s);
int note_compare(const void *a, const void *b);
void notify_verbose_log(t_notify *x, const char *fmt, ...);
void notify_local_bar_length(t_notify *x, double f);
long notify_get_bar_length(t_notify *x);

void ext_main(void *r) {
    t_class *c;

    common_symbols_init();

    c = class_new("notify", (method)notify_new, (method)notify_free, sizeof(t_notify), 0L, A_GIMME, 0);
    class_addmethod(c, (method)notify_bang, "bang", 0);
    class_addmethod(c, (method)notify_fill, "fill", 0);
    class_addmethod(c, (method)notify_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)notify_local_bar_length, "ft1", A_FLOAT, 0);
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

long notify_get_bar_length(t_notify *x) {
    if (x->local_bar_length > 0) {
        return (long)x->local_bar_length;
    }

    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        notify_verbose_log(x, "bar buffer~ not found");
        return 0;
    }

    long bar_length = 0;
    critical_enter(0);
    float *samples = buffer_locksamples(b);
    if (samples) {
        if (buffer_getframecount(b) > 0) {
            bar_length = (long)samples[0];
        }
        buffer_unlocksamples(b);
        critical_exit(0);
    } else {
        critical_exit(0);
    }

    if (bar_length > 0) {
        x->local_bar_length = (double)bar_length;
        notify_verbose_log(x, "thread %ld: bar_length changed to %ld", x->instance_id, bar_length);
    }

    return bar_length;
}

void notify_local_bar_length(t_notify *x, double f) {
    if (f <= 0) {
        x->local_bar_length = 0;
    } else {
        x->local_bar_length = f;
    }
    notify_verbose_log(x, "thread %ld: bar_length changed to %ld", x->instance_id, (long)x->local_bar_length);
    notify_verbose_log(x, "Local bar length set to: %.2f", f);
}

void *notify_new(t_symbol *s, long argc, t_atom *argv) {
    t_notify *x = (t_notify *)object_alloc(notify_class);
    if (x) {
        x->dict_name = gensym("");
        x->verbose = 0;
        x->out_verbose = NULL;
        x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        x->local_bar_length = 0;
        x->instance_id = 1000 + (rand() % 9000);

        attr_args_process(x, argc, argv);

        // Outlets are created from right to left
        if (x->verbose) {
            x->out_verbose = outlet_new((t_object *)x, NULL);
        }
        x->out_descript = outlet_new((t_object *)x, NULL);
        x->out_palette = outlet_new((t_object *)x, NULL);
        x->out_track = outlet_new((t_object *)x, NULL);
        x->out_offset = outlet_new((t_object *)x, NULL);
        x->out_abs_score = outlet_new((t_object *)x, NULL);

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->dict_name = atom_getsym(argv);
        }

        floatin((t_object *)x, 1);
    }
    return (x);
}

void notify_free(t_notify *x) {
    if (x->buffer_ref) {
        object_free(x->buffer_ref);
    }
}

t_max_err notify_notify(t_notify *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    return buffer_ref_notify(x->buffer_ref, s, msg, sender, data);
}

int note_compare(const void *a, const void *b) {
    t_note *noteA = (t_note *)a;
    t_note *noteB = (t_note *)b;
    if (noteA->absolute < noteB->absolute) return -1;
    if (noteA->absolute > noteB->absolute) return 1;
    return 0;
}

void notify_fill(t_notify *x) {
    t_dictionary *dict = dictobj_findregistered_retain(x->dict_name);
    if (!dict) {
        object_error((t_object *)x, "could not find dictionary named %s", x->dict_name->s_name);
        return;
    }

    long num_tracks = 0;
    t_symbol **track_keys = NULL;
    dictionary_getkeys(dict, &num_tracks, &track_keys);

    double max_bar_all = -1.0;
    for (long i = 0; i < num_tracks; i++) {
        t_dictionary *track_dict = NULL;
        dictionary_getdictionary(dict, track_keys[i], (t_object **)&track_dict);
        if (!track_dict) continue;

        long num_bars = 0;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);
        for (long j = 0; j < num_bars; j++) {
            double bar_ts = atof(bar_keys[j]->s_name);
            if (bar_ts > max_bar_all) max_bar_all = bar_ts;
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }

    if (max_bar_all < 0) {
        if (track_keys) sysmem_freeptr(track_keys);
        object_release((t_object *)dict);
        return;
    }

    t_note *all_notes = NULL;
    long total_notes = 0;
    long notes_capacity = 100;
    all_notes = (t_note *)sysmem_newptr(sizeof(t_note) * notes_capacity);

    for (long i = 0; i < num_tracks; i++) {
        t_symbol *track_sym = track_keys[i];
        t_dictionary *track_dict = NULL;
        dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict);
        if (!track_dict) continue;

        double max_bar_this = -1.0;
        long num_bars = 0;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);
        for (long j = 0; j < num_bars; j++) {
            double bar_ts = atof(bar_keys[j]->s_name);
            if (bar_ts > max_bar_this) max_bar_this = bar_ts;
        }

        if (max_bar_this < max_bar_all && max_bar_this > 0) {
            for (long n = 1; ; n++) {
                int notes_added_this_pass = 0;
                for (long j = 0; j < num_bars; j++) {
                    t_symbol *bar_sym = bar_keys[j];
                    double bar_ts = atof(bar_sym->s_name);
                    double synth_bar_ts = bar_ts + n * max_bar_this;

                    t_dictionary *bar_dict = NULL;
                    dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
                    if (!bar_dict) continue;

                    double offset = 0;
                    t_atomarray *offset_aa = NULL;
                    t_atom offset_atom;
                    if (dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_aa) == MAX_ERR_NONE && offset_aa) {
                        t_atom o_atom;
                        if (atomarray_getindex(offset_aa, 0, &o_atom) == MAX_ERR_NONE) offset = atom_getfloat(&o_atom);
                    } else if (dictionary_getatom(bar_dict, gensym("offset"), &offset_atom) == MAX_ERR_NONE) {
                        offset = atom_getfloat(&offset_atom);
                    }

                    t_symbol *palette = gensym("");
                    t_atomarray *palette_aa = NULL;
                    t_atom palette_atom;
                    if (dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_aa) == MAX_ERR_NONE && palette_aa) {
                        t_atom p_atom;
                        if (atomarray_getindex(palette_aa, 0, &p_atom) == MAX_ERR_NONE) palette = atom_getsym(&p_atom);
                    } else if (dictionary_getatom(bar_dict, gensym("palette"), &palette_atom) == MAX_ERR_NONE) {
                        palette = atom_getsym(&palette_atom);
                    }

                    t_atomarray *absolutes_aa = NULL;
                    if (dictionary_getatomarray(bar_dict, gensym("absolutes"), (t_object **)&absolutes_aa) == MAX_ERR_NONE && absolutes_aa) {
                        long aa_len = 0;
                        t_atom *aa_atoms = NULL;
                        atomarray_getatoms(absolutes_aa, &aa_len, &aa_atoms);
                        t_atomarray *scores_aa = NULL;
                        long scores_len = 0;
                        t_atom *scores_atoms = NULL;
                        if (dictionary_getatomarray(bar_dict, gensym("scores"), (t_object **)&scores_aa) == MAX_ERR_NONE && scores_aa) {
                            atomarray_getatoms(scores_aa, &scores_len, &scores_atoms);
                        }
                        long num_entries = aa_len < scores_len ? aa_len : scores_len;
                        for (long k = 0; k < num_entries; k++) {
                            double orig_abs = atom_getfloat(&aa_atoms[k]);
                            double synth_abs = orig_abs + n * max_bar_this;
                            if (synth_abs < max_bar_all) {
                                if (total_notes >= notes_capacity) {
                                    notes_capacity *= 2;
                                    all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                                }
                                all_notes[total_notes].absolute = synth_abs;
                                all_notes[total_notes].score = atom_getfloat(&scores_atoms[k]);
                                all_notes[total_notes].offset = offset;
                                all_notes[total_notes].bar_ts = synth_bar_ts;
                                all_notes[total_notes].track = track_sym;
                                all_notes[total_notes].palette = palette;
                                total_notes++;
                                notes_added_this_pass++;
                            }
                        }
                    } else {
                        t_atom absolute_atom;
                        if (dictionary_getatom(bar_dict, gensym("absolutes"), &absolute_atom) == MAX_ERR_NONE) {
                            double orig_abs = atom_getfloat(&absolute_atom);
                            double synth_abs = orig_abs + n * max_bar_this;
                            if (synth_abs < max_bar_all) {
                                if (total_notes >= notes_capacity) {
                                    notes_capacity *= 2;
                                    all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                                }
                                all_notes[total_notes].absolute = synth_abs;
                                t_atom score_atom;
                                if (dictionary_getatom(bar_dict, gensym("scores"), &score_atom) == MAX_ERR_NONE) {
                                    all_notes[total_notes].score = atom_getfloat(&score_atom);
                                } else {
                                    all_notes[total_notes].score = 0;
                                }
                                all_notes[total_notes].offset = offset;
                                all_notes[total_notes].bar_ts = synth_bar_ts;
                                all_notes[total_notes].track = track_sym;
                                all_notes[total_notes].palette = palette;
                                total_notes++;
                                notes_added_this_pass++;
                            }
                        }
                    }
                }
                if (notes_added_this_pass == 0) break;
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }
    if (track_keys) sysmem_freeptr(track_keys);

    if (total_notes > 1) qsort(all_notes, total_notes, sizeof(t_note), note_compare);

    for (long i = 0; i < total_notes; i++) {
        outlet_anything(x->out_palette, all_notes[i].palette, 0, NULL);
        if (all_notes[i].track == NULL || all_notes[i].track == gensym("")) {
            outlet_int(x->out_track, 0);
        } else {
            outlet_int(x->out_track, atol(all_notes[i].track->s_name));
        }
        outlet_float(x->out_offset, all_notes[i].offset);
        t_atom list_atoms[2];
        atom_setfloat(&list_atoms[0], all_notes[i].absolute);
        atom_setfloat(&list_atoms[1], all_notes[i].score);
        outlet_list(x->out_abs_score, NULL, 2, list_atoms);
    }

    if (all_notes) sysmem_freeptr(all_notes);
    object_release((t_object *)dict);
}

void notify_bang(t_notify *x) {
    t_dictionary *dict = dictobj_findregistered_retain(x->dict_name);
    if (!dict) {
        object_error((t_object *)x, "could not find dictionary named %s", x->dict_name->s_name);
        return;
    }

    long bar_length = notify_get_bar_length(x);

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
            double bar_ts = atof(bar_sym->s_name);

            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(track_dict, bar_sym, (t_object **)&bar_dict);
            if (!bar_dict) continue;

            double offset = 0;
            t_atomarray *offset_aa = NULL;
            t_atom offset_atom;
            if (dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_aa) == MAX_ERR_NONE && offset_aa) {
                t_atom o_atom;
                if (atomarray_getindex(offset_aa, 0, &o_atom) == MAX_ERR_NONE) offset = atom_getfloat(&o_atom);
            } else if (dictionary_getatom(bar_dict, gensym("offset"), &offset_atom) == MAX_ERR_NONE) {
                offset = atom_getfloat(&offset_atom);
            }

            t_symbol *palette = gensym("");
            t_atomarray *palette_aa = NULL;
            t_atom palette_atom;
            if (dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_aa) == MAX_ERR_NONE && palette_aa) {
                t_atom p_atom;
                if (atomarray_getindex(palette_aa, 0, &p_atom) == MAX_ERR_NONE) palette = atom_getsym(&p_atom);
            } else if (dictionary_getatom(bar_dict, gensym("palette"), &palette_atom) == MAX_ERR_NONE) {
                palette = atom_getsym(&palette_atom);
            }

            // Output descript message for THIS bar
            t_atom descript_list[3];
            long track_val = (track_sym == NULL || track_sym == gensym("")) ? 0 : atol(track_sym->s_name);
            atom_setlong(&descript_list[0], track_val);
            atom_setfloat(&descript_list[1], bar_ts);
            atom_setfloat(&descript_list[2], 0.0);
            outlet_anything(x->out_descript, palette, 3, descript_list);

            // Handle NEXT bar descript
            if (bar_length > 0) {
                double next_bar_ts = bar_ts + bar_length;
                char next_bar_str[64];
                snprintf(next_bar_str, 64, "%ld", (long)next_bar_ts);
                if (!dictionary_hasentry(track_dict, gensym(next_bar_str))) {
                    atom_setfloat(&descript_list[1], next_bar_ts);
                    outlet_anything(x->out_descript, palette, 3, descript_list);
                }
            }

            long num_entries = 0;
            t_atomarray *absolutes_aa = NULL;
            if (dictionary_getatomarray(bar_dict, gensym("absolutes"), (t_object **)&absolutes_aa) == MAX_ERR_NONE && absolutes_aa) {
                long aa_len = 0;
                t_atom *aa_atoms = NULL;
                atomarray_getatoms(absolutes_aa, &aa_len, &aa_atoms);
                t_atomarray *scores_aa_check = NULL;
                if (dictionary_getatomarray(bar_dict, gensym("scores"), (t_object **)&scores_aa_check) == MAX_ERR_NONE && scores_aa_check) {
                    long scores_len = 0;
                    t_atom *scores_atoms = NULL;
                    atomarray_getatoms(scores_aa_check, &scores_len, &scores_atoms);
                    num_entries = aa_len < scores_len ? aa_len : scores_len;
                    for (long k = 0; k < num_entries; k++) {
                        if (total_notes >= notes_capacity) {
                            notes_capacity *= 2;
                            all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                        }
                        all_notes[total_notes].absolute = atom_getfloat(&aa_atoms[k]);
                        all_notes[total_notes].score = atom_getfloat(&scores_atoms[k]);
                        all_notes[total_notes].offset = offset;
                        all_notes[total_notes].bar_ts = bar_ts;
                        all_notes[total_notes].track = track_sym;
                        all_notes[total_notes].palette = palette;
                        total_notes++;
                    }
                }
            } else {
                t_atom absolute_atom;
                if (dictionary_getatom(bar_dict, gensym("absolutes"), &absolute_atom) == MAX_ERR_NONE) {
                    if (total_notes >= notes_capacity) {
                        notes_capacity *= 2;
                        all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                    }
                    all_notes[total_notes].absolute = atom_getfloat(&absolute_atom);
                    t_atom score_atom;
                    if (dictionary_getatom(bar_dict, gensym("scores"), &score_atom) == MAX_ERR_NONE) {
                        all_notes[total_notes].score = atom_getfloat(&score_atom);
                    } else {
                        all_notes[total_notes].score = 0;
                    }
                    all_notes[total_notes].offset = offset;
                    all_notes[total_notes].bar_ts = bar_ts;
                    all_notes[total_notes].track = track_sym;
                    all_notes[total_notes].palette = palette;
                    total_notes++;
                }
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }
    if (track_keys) sysmem_freeptr(track_keys);

    if (total_notes > 1) qsort(all_notes, total_notes, sizeof(t_note), note_compare);

    dictionary_clear(dict);
    notify_verbose_log(x, "Dictionary cleared.");

    for (long i = 0; i < total_notes; i++) {
        outlet_anything(x->out_palette, all_notes[i].palette, 0, NULL);
        if (all_notes[i].track == NULL || all_notes[i].track == gensym("")) {
            outlet_int(x->out_track, 0);
        } else {
            outlet_int(x->out_track, atol(all_notes[i].track->s_name));
        }
        outlet_float(x->out_offset, all_notes[i].offset);
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
        if (a == 0) sprintf(s, "Inlet 1: (bang) dump dictionary, (fill) specialized dump");
        else if (a == 1) sprintf(s, "Inlet 2: (float) Local Bar Length");
    } else {
        if (x->verbose) {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: Abs Timestamp and Score (list)"); break;
                case 1: sprintf(s, "Outlet 2: Offset (float)"); break;
                case 2: sprintf(s, "Outlet 3: Track (int)"); break;
                case 3: sprintf(s, "Outlet 4: Palette (symbol)"); break;
                case 4: sprintf(s, "Outlet 5: Descript List (Batch at Start): <palette> <track> <bar_ts> 0.0"); break;
                case 5: sprintf(s, "Outlet 6: Verbose Logging"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: Abs Timestamp and Score (list)"); break;
                case 1: sprintf(s, "Outlet 2: Offset (float)"); break;
                case 2: sprintf(s, "Outlet 3: Track (int)"); break;
                case 3: sprintf(s, "Outlet 4: Palette (symbol)"); break;
                case 4: sprintf(s, "Outlet 5: Descript List (Batch at Start): <palette> <track> <bar_ts> 0.0"); break;
            }
        }
    }
}

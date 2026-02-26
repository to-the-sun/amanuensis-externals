#include "ext.h"
#include "ext_obex.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_atomarray.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
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
    double original_absolute;
} t_note;

typedef struct _notify {
    t_object s_obj;
    t_symbol *dict_name;
    long log;
    void *out_abs_score; // Outlet 1 (Leftmost, Index 0)
    void *out_offset;    // Outlet 2 (Index 1)
    void *out_track;     // Outlet 3 (Index 2)
    void *out_palette;   // Outlet 4 (Index 3)
    void *out_log;   // Outlet 5 (Rightmost, optional, Index 4)
    long instance_id;
    void *q_work;
    long busy;
    long async;
    int job; // 1 = bang, 2 = fill
    t_buffer_ref *bar_ref;
    long bar_connected;
} t_notify;

t_class *notify_class;

// Function prototypes
void *notify_new(t_symbol *s, long argc, t_atom *argv);
void notify_free(t_notify *x);
void notify_bang(t_notify *x);
void notify_fill(t_notify *x);
void notify_qwork(t_notify *x);
void notify_do_bang(t_notify *x);
void notify_do_fill(t_notify *x);
void notify_assist(t_notify *x, void *b, long m, long a, char *s);
int note_compare(const void *a, const void *b);
void notify_log(t_notify *x, const char *fmt, ...);

void ext_main(void *r) {
    t_class *c;

    common_symbols_init();

    c = class_new("notify", (method)notify_new, (method)notify_free, sizeof(t_notify), 0L, A_GIMME, 0);
    class_addmethod(c, (method)notify_bang, "bang", 0);
    class_addmethod(c, (method)notify_fill, "fill", 0);
    class_addmethod(c, (method)notify_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_notify, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_LONG(c, "async", 0, t_notify, async);
    CLASS_ATTR_STYLE_LABEL(c, "async", 0, "onoff", "Asynchronous Execution");
    CLASS_ATTR_DEFAULT(c, "async", 0, "0");

    class_register(CLASS_BOX, c);
    notify_class = c;
}

void notify_log(t_notify *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->out_log, x->log, "notify", fmt, args);
    va_end(args);
}

void *notify_new(t_symbol *s, long argc, t_atom *argv) {
    t_notify *x = (t_notify *)object_alloc(notify_class);
    if (x) {
        x->dict_name = gensym("");
        x->log = 0;
        x->async = 0;
        x->out_log = NULL;
        x->instance_id = 1000 + (rand() % 9000);
        x->q_work = qelem_new(x, (method)notify_qwork);
        x->busy = 0;
        x->job = 0;
        x->bar_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        x->bar_connected = 0;

        attr_args_process(x, argc, argv);

        // Outlets are created from right to left
        if (x->log) {
            x->out_log = outlet_new((t_object *)x, NULL);
        }

        t_buffer_obj *bar_obj = buffer_ref_getobject(x->bar_ref);
        if (!bar_obj) {
            object_error((t_object *)x, "could not find buffer named bar");
        } else {
            x->bar_connected = 1;
            notify_log(x, "Successfully connected to buffer 'bar'");
        }
        x->out_palette = outlet_new((t_object *)x, NULL);
        x->out_track = outlet_new((t_object *)x, NULL);
        x->out_offset = outlet_new((t_object *)x, NULL);
        x->out_abs_score = outlet_new((t_object *)x, NULL);

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->dict_name = atom_getsym(argv);
        }
    }
    return (x);
}

void notify_free(t_notify *x) {
    if (x->q_work) {
        qelem_free(x->q_work);
    }
    object_free(x->bar_ref);
}

int note_compare(const void *a, const void *b) {
    t_note *noteA = (t_note *)a;
    t_note *noteB = (t_note *)b;

    // Sort solely by absolute timestamp
    if (noteA->absolute < noteB->absolute) return -1;
    if (noteA->absolute > noteB->absolute) return 1;

    return 0;
}

void notify_fill(t_notify *x) {
    if (x->async) {
        if (x->busy) {
            notify_log(x, "notify is busy, ignoring fill");
            return;
        }
        x->job = 2;
        x->busy = 1;
        qelem_set(x->q_work);
        notify_log(x, "Deferred fill to Main thread.");
    } else {
        notify_do_fill(x);
    }
}

void notify_do_fill(t_notify *x) {
    t_dictionary *dict = dictobj_findregistered_retain(x->dict_name);
    if (!dict) {
        object_error((t_object *)x, "could not find dictionary named %s", x->dict_name->s_name);
        return;
    }

    double bar_length = 0;
    t_buffer_obj *bar_obj = buffer_ref_getobject(x->bar_ref);
    if (!bar_obj) {
        object_error((t_object *)x, "could not find buffer named bar");
        buffer_ref_set(x->bar_ref, _sym_nothing);
        buffer_ref_set(x->bar_ref, gensym("bar"));
        x->bar_connected = 0;
        bar_obj = buffer_ref_getobject(x->bar_ref); // Try again after kick
    }

    if (!bar_obj) {
        object_release((t_object *)dict);
        return;
    } else {
        if (!x->bar_connected) {
            x->bar_connected = 1;
            notify_log(x, "Successfully connected to buffer 'bar'");
        }
        float *tab = buffer_locksamples(bar_obj);
        if (tab) {
            bar_length = tab[0];
            buffer_unlocksamples(bar_obj);
        }
    }

    long num_tracks = 0;
    t_symbol **track_keys = NULL;
    dictionary_getkeys(dict, &num_tracks, &track_keys);

    double max_bar_all = -1.0;
    t_symbol *longest_track = NULL;
    for (long i = 0; i < num_tracks; i++) {
        t_dictionary *track_dict = NULL;
        dictionary_getdictionary(dict, track_keys[i], (t_object **)&track_dict);
        if (!track_dict) continue;

        long num_bars = 0;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);
        for (long j = 0; j < num_bars; j++) {
            double bar_ts = atof(bar_keys[j]->s_name);
            if (bar_ts > max_bar_all) {
                max_bar_all = bar_ts;
                longest_track = track_keys[i];
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }

    if (max_bar_all < 0) {
        if (track_keys) sysmem_freeptr(track_keys);
        object_release((t_object *)dict);
        outlet_bang(x->out_abs_score);
        return;
    }

    max_bar_all += bar_length; // Incorporate bar_length

    t_note *all_notes = NULL;
    long total_notes = 0;
    long notes_capacity = 100;
    long total_synthetic_added = 0;
    all_notes = (t_note *)sysmem_newptr(sizeof(t_note) * notes_capacity);

    notify_log(x, "Fill: processing %ld tracks. Global reach %.2f ms (including bar_length %.2f) defined by track %s.", num_tracks, max_bar_all, bar_length, longest_track ? longest_track->s_name : "none");

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
        if (max_bar_this >= 0) max_bar_this += bar_length; // Incorporate bar_length

        // Log original notes for this track before synthesis
        if (x->log && x->out_log) {
            notify_log(x, "Track %s: Listing original notes.", track_sym->s_name);
            for (long j = 0; j < num_bars; j++) {
                t_symbol *bar_sym = bar_keys[j];
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

                t_atom *aa_atoms = NULL;
                long aa_len = 0;
                t_atomarray *absolutes_aa = NULL;
                t_atom absolute_atom;
                if (dictionary_getatomarray(bar_dict, gensym("absolutes"), (t_object **)&absolutes_aa) == MAX_ERR_NONE && absolutes_aa) {
                    atomarray_getatoms(absolutes_aa, &aa_len, &aa_atoms);
                } else if (dictionary_getatom(bar_dict, gensym("absolutes"), &absolute_atom) == MAX_ERR_NONE) {
                    aa_atoms = &absolute_atom;
                    aa_len = 1;
                }

                if (aa_len > 0) {
                    t_atom *scores_atoms = NULL;
                    long scores_len = 0;
                    t_atomarray *scores_aa = NULL;
                    t_atom score_atom;
                    if (dictionary_getatomarray(bar_dict, gensym("scores"), (t_object **)&scores_aa) == MAX_ERR_NONE && scores_aa) {
                        atomarray_getatoms(scores_aa, &scores_len, &scores_atoms);
                    } else if (dictionary_getatom(bar_dict, gensym("scores"), &score_atom) == MAX_ERR_NONE) {
                        scores_atoms = &score_atom;
                        scores_len = 1;
                    }

                    long num_notes = (aa_len < scores_len) ? aa_len : scores_len;
                    for (long k = 0; k < num_notes; k++) {
                        notify_log(x, "Track %s: Original Note: palette %s, abs %.2f, score %.2f, offset %.2f",
                            track_sym->s_name, palette->s_name, atom_getfloat(&aa_atoms[k]), atom_getfloat(&scores_atoms[k]), offset);
                    }
                }
            }
        }

        if (max_bar_this == max_bar_all) {
            notify_log(x, "Track %s is a reference track (length %.2f). No duplication needed.", track_sym->s_name, max_bar_all);
        } else if (max_bar_this < max_bar_all && max_bar_this > 0) {
            notify_log(x, "Track %s (length %.2f) < Global Reach (%.2f). Growing with period %.2f.", track_sym->s_name, max_bar_this, max_bar_all, max_bar_this);
            for (long n = 1; ; n++) {
                int notes_added_this_pass = 0;
                for (long j = 0; j < num_bars; j++) {
                    t_symbol *bar_sym = bar_keys[j];
                    double bar_ts = atof(bar_sym->s_name);
                    double synth_bar_ts = bar_ts + n * max_bar_this;

                    if (synth_bar_ts <= max_bar_this) continue;
                    if (synth_bar_ts >= max_bar_all) continue; // Non-inclusive filtering

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

                    t_atom *aa_atoms = NULL;
                    long aa_len = 0;
                    t_atomarray *absolutes_aa = NULL;
                    t_atom absolute_atom;

                    if (dictionary_getatomarray(bar_dict, gensym("absolutes"), (t_object **)&absolutes_aa) == MAX_ERR_NONE && absolutes_aa) {
                        atomarray_getatoms(absolutes_aa, &aa_len, &aa_atoms);
                    } else if (dictionary_getatom(bar_dict, gensym("absolutes"), &absolute_atom) == MAX_ERR_NONE) {
                        aa_atoms = &absolute_atom;
                        aa_len = 1;
                    }

                    if (aa_len > 0) {
                        t_atom *scores_atoms = NULL;
                        long scores_len = 0;
                        t_atomarray *scores_aa = NULL;
                        t_atom score_atom;

                        if (dictionary_getatomarray(bar_dict, gensym("scores"), (t_object **)&scores_aa) == MAX_ERR_NONE && scores_aa) {
                            atomarray_getatoms(scores_aa, &scores_len, &scores_atoms);
                        } else if (dictionary_getatom(bar_dict, gensym("scores"), &score_atom) == MAX_ERR_NONE) {
                            scores_atoms = &score_atom;
                            scores_len = 1;
                        }

                        long num_notes = (aa_len < scores_len) ? aa_len : scores_len;
                        for (long k = 0; k < num_notes; k++) {
                            double orig_abs = atom_getfloat(&aa_atoms[k]);
                            double synth_abs = orig_abs + n * max_bar_this;
                            if (total_notes >= notes_capacity) {
                                notes_capacity *= 2;
                                all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                            }
                            notify_log(x, "Synthesizing: %.2f + (%ld * %.2f) = %.2f (Original Note: palette %s, score %.2f, offset %.2f)",
                                orig_abs, n, max_bar_this, synth_abs, palette->s_name, atom_getfloat(&scores_atoms[k]), offset);
                            all_notes[total_notes].absolute = synth_abs;
                            all_notes[total_notes].original_absolute = orig_abs;
                            all_notes[total_notes].score = atom_getfloat(&scores_atoms[k]);
                            all_notes[total_notes].offset = offset;
                            all_notes[total_notes].bar_ts = synth_bar_ts;
                            all_notes[total_notes].track = track_sym;
                            all_notes[total_notes].palette = palette;
                            total_notes++;
                            notes_added_this_pass++;
                            total_synthetic_added++;
                        }
                    }
                }
                if (notes_added_this_pass == 0) break;
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }
    if (track_keys) sysmem_freeptr(track_keys);

    notify_log(x, "Fill: added %ld total synthetic notes to manifest.", total_synthetic_added);

    if (total_notes > 1) qsort(all_notes, total_notes, sizeof(t_note), note_compare);

    // Verbose Manifest Logging
    if (x->log && x->out_log) {
        for (long i = 0; i < total_notes; i++) {
            notify_log(x, "Note Manifest: palette %s, absolute %.2f (orig: %.2f), score %.2f, track %s, offset %.2f",
                all_notes[i].palette->s_name,
                all_notes[i].absolute,
                all_notes[i].original_absolute,
                all_notes[i].score,
                (all_notes[i].track && all_notes[i].track != gensym("")) ? all_notes[i].track->s_name : "0",
                all_notes[i].offset);
        }
    }

    for (long i = 0; i < total_notes; i++) {
        outlet_anything(x->out_palette, all_notes[i].palette, 0, NULL);
        if (all_notes[i].track == NULL || all_notes[i].track == gensym("")) {
            outlet_int(x->out_track, 0);
        } else {
            outlet_int(x->out_track, atol(all_notes[i].track->s_name));
        }
        outlet_float(x->out_offset, all_notes[i].offset);
        t_atom list_atoms[3];
        atom_setfloat(&list_atoms[0], all_notes[i].absolute);
        atom_setfloat(&list_atoms[1], all_notes[i].score);
        atom_setfloat(&list_atoms[2], all_notes[i].original_absolute);
        outlet_list(x->out_abs_score, NULL, 3, list_atoms);
    }
    outlet_bang(x->out_abs_score);

    if (all_notes) sysmem_freeptr(all_notes);
    object_release((t_object *)dict);
}

void notify_bang(t_notify *x) {
    if (x->async) {
        if (x->busy) {
            notify_log(x, "notify is busy, ignoring bang");
            return;
        }
        x->job = 1;
        x->busy = 1;
        qelem_set(x->q_work);
        notify_log(x, "Deferred bang to Main thread.");
    } else {
        notify_do_bang(x);
    }
}

void notify_qwork(t_notify *x) {
    if (x->job == 1) {
        notify_do_bang(x);
    } else if (x->job == 2) {
        notify_do_fill(x);
    }
    x->busy = 0;
    x->job = 0;
}

void notify_do_bang(t_notify *x) {
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

            t_atom *aa_atoms = NULL;
            long aa_len = 0;
            t_atomarray *absolutes_aa = NULL;
            t_atom absolute_atom;

            // Robust Note Collection
            if (dictionary_getatomarray(bar_dict, gensym("absolutes"), (t_object **)&absolutes_aa) == MAX_ERR_NONE && absolutes_aa) {
                atomarray_getatoms(absolutes_aa, &aa_len, &aa_atoms);
            } else if (dictionary_getatom(bar_dict, gensym("absolutes"), &absolute_atom) == MAX_ERR_NONE) {
                aa_atoms = &absolute_atom;
                aa_len = 1;
            }

            if (aa_len > 0) {
                t_atom *scores_atoms = NULL;
                long scores_len = 0;
                t_atomarray *scores_aa = NULL;
                t_atom score_atom;

                if (dictionary_getatomarray(bar_dict, gensym("scores"), (t_object **)&scores_aa) == MAX_ERR_NONE && scores_aa) {
                    atomarray_getatoms(scores_aa, &scores_len, &scores_atoms);
                } else if (dictionary_getatom(bar_dict, gensym("scores"), &score_atom) == MAX_ERR_NONE) {
                    scores_atoms = &score_atom;
                    scores_len = 1;
                }

                long num_notes = (aa_len < scores_len) ? aa_len : scores_len;
                for (long k = 0; k < num_notes; k++) {
                    if (total_notes >= notes_capacity) {
                        notes_capacity *= 2;
                        all_notes = (t_note *)sysmem_resizeptr(all_notes, sizeof(t_note) * notes_capacity);
                    }
                    all_notes[total_notes].absolute = atom_getfloat(&aa_atoms[k]);
                    all_notes[total_notes].original_absolute = all_notes[total_notes].absolute;
                    all_notes[total_notes].score = atom_getfloat(&scores_atoms[k]);
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

    // Sort everything
    if (total_notes > 1) qsort(all_notes, total_notes, sizeof(t_note), note_compare);

    // Verbose Manifest Logging
    if (x->log && x->out_log) {
        for (long i = 0; i < total_notes; i++) {
            notify_log(x, "Note Manifest: palette %s, absolute %.2f (orig: %.2f), score %.2f, track %s, offset %.2f, bar_ts %.2f",
                all_notes[i].palette->s_name,
                all_notes[i].absolute,
                all_notes[i].original_absolute,
                all_notes[i].score,
                (all_notes[i].track && all_notes[i].track != gensym("")) ? all_notes[i].track->s_name : "0",
                all_notes[i].offset,
                all_notes[i].bar_ts);
        }
    }

    dictionary_clear(dict);
    notify_log(x, "Dictionary cleared.");

    // Output sorted notes
    for (long i = 0; i < total_notes; i++) {
        outlet_anything(x->out_palette, all_notes[i].palette, 0, NULL);
        if (all_notes[i].track == NULL || all_notes[i].track == gensym("")) {
            outlet_int(x->out_track, 0);
        } else {
            outlet_int(x->out_track, atol(all_notes[i].track->s_name));
        }
        outlet_float(x->out_offset, all_notes[i].offset);
        t_atom list_atoms[3];
        atom_setfloat(&list_atoms[0], all_notes[i].absolute);
        atom_setfloat(&list_atoms[1], all_notes[i].score);
        atom_setfloat(&list_atoms[2], all_notes[i].original_absolute);
        outlet_list(x->out_abs_score, NULL, 3, list_atoms);
    }
    outlet_bang(x->out_abs_score);

    if (all_notes) sysmem_freeptr(all_notes);
    object_release((t_object *)dict);
}

void notify_assist(t_notify *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (bang) aggregate and sort notes, (fill) synthesized fill and sort. Supports @async deferral.");
    } else {
        if (x->log) {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: [synth_abs, score, orig_abs] (list). Sorted chronologically. Sends bang when finished."); break;
                case 1: sprintf(s, "Outlet 2: Note Offset (float)"); break;
                case 2: sprintf(s, "Outlet 3: Track ID (int)"); break;
                case 3: sprintf(s, "Outlet 4: Palette Name (symbol)"); break;
                case 4: sprintf(s, "Outlet 5: Logging and Status messages"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "Outlet 1: [synth_abs, score, orig_abs] (list). Sorted chronologically. Sends bang when finished."); break;
                case 1: sprintf(s, "Outlet 2: Note Offset (float)"); break;
                case 2: sprintf(s, "Outlet 3: Track ID (int)"); break;
                case 3: sprintf(s, "Outlet 4: Palette Name (symbol)"); break;
            }
        }
    }
}

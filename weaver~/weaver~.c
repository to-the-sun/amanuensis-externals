#include "ext.h"
#include "ext_obex.h"
#include "ext_hashtab.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "ext_dictionary.h"
#include "ext_dictobj.h"
#include "ext_atomarray.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include "../shared/crossfade.h"
#include "../shared/visualize.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct _bar_cache {
    double value;
    t_symbol *sym;
} t_bar_cache;

#define TYPE_DATA 0
#define TYPE_SWEEP 1
#define TYPE_LOOP 2

typedef struct _fifo_entry {
    t_bar_cache bar;
    double range_end; // For SWEEP
    long type; // 0 for DATA, 1 for SWEEP
    long track_id;
    int no_crossfade;
} t_fifo_entry;

typedef struct _weaver_track {
    t_crossfade_state xf;
    t_symbol *palette[2];
    double offset[2];
    double control;
    int busy;
    t_buffer_ref *src_refs[2];
    t_buffer_ref *dest_ref;
    long dest_found;
    long dest_warn_sent;
    long src_found[2];
    long src_error_sent[2];
    double track_length;
    double last_track_scan;
    double last_visualize_ms;
    double *schedule;
    long schedule_count;
    long schedule_size;
    t_hashtab *silence_caps;
    long next_schedule_idx;
} t_weaver_track;

typedef struct _weaver {
    t_pxobject t_obj;
    t_symbol *poly_prefix;
    long log;
    long visualize;
    void *log_outlet;
    void *loop_outlet;
    t_buffer_ref *bar_buffer_ref;
    t_buffer_ref *track1_ref;

    t_symbol *audio_dict_name;
    double last_scan_val;
    t_fifo_entry hit_bars[4096];
    int fifo_head;
    int fifo_tail;
    t_qelem *audio_qelem;
    t_critical lock;
    long max_tracks;
    t_weaver_track *track_cache[256];
    long track_cache_count;
    void *proxy;
    long proxy_id;
    double cached_bar_len;
    long bar_found;
    long bar_error_sent;
    long dict_found;
    long dict_error_sent;
    long poly_found;
    long poly_warn_sent;

    t_hashtab *track_states;
    double low_ms;
    double high_ms;
} t_weaver;

int compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double*)a;
    double arg2 = *(const double*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

void *weaver_new(t_symbol *s, long argc, t_atom *argv);
void weaver_free(t_weaver *x);
void weaver_list(t_weaver *x, t_symbol *s, long argc, t_atom *argv);
void weaver_anything(t_weaver *x, t_symbol *s, long argc, t_atom *argv);
t_max_err weaver_notify(t_weaver *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void weaver_assist(t_weaver *x, void *b, long m, long a, char *s);
void weaver_log(t_weaver *x, const char *fmt, ...);
void weaver_process_data(t_weaver *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms, int no_crossfade, int is_bar_0, int skip_trigger);
void weaver_check_attachments(t_weaver *x);
double weaver_get_bar_length(t_weaver *x);
void weaver_dsp64(t_weaver *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void weaver_perform64(t_weaver *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void weaver_audio_qtask(t_weaver *x);
void weaver_schedule_silence(t_weaver *x, t_weaver_track *tr, long track_id, double ms);
t_weaver_track *weaver_get_track_state(t_weaver *x, t_atom_long track_id);
void weaver_clear_track_states(t_weaver *x);
void weaver_update_track_cache(t_weaver *x);
void weaver_track_update_schedule(t_weaver *x, t_weaver_track *tr, long track_id);
void weaver_update_all_schedules(t_weaver *x);

static t_class *weaver_class;

void weaver_track_update_schedule(t_weaver *x, t_weaver_track *tr, long track_id) {
    if (!tr) return;

    t_dictionary *dict = dictobj_findregistered_retain(x->audio_dict_name);
    t_dictionary *track_dict = NULL;
    char tstr[64];
    snprintf(tstr, 64, "%ld", track_id);
    t_symbol *track_sym = gensym(tstr);

    long num_bars = 0;
    t_symbol **bar_keys = NULL;

    if (dict && dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict) == MAX_ERR_NONE && track_dict) {
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);
    }

    long num_caps = 0;
    t_symbol **cap_keys = NULL;
    if (tr->silence_caps) {
        hashtab_getkeys(tr->silence_caps, &num_caps, &cap_keys);
    }

    long total_potential = num_bars + num_caps;
    if (total_potential == 0) {
        critical_enter(x->lock);
        tr->schedule_count = 0;
        critical_exit(x->lock);
        if (bar_keys) dictionary_freekeys(track_dict, num_bars, bar_keys);
        if (cap_keys) sysmem_freeptr(cap_keys);
        if (dict) dictobj_release(dict);
        return;
    }

    double *temp_schedule = (double *)sysmem_newptr(total_potential * sizeof(double));
    long actual_count = 0;

    for (long i = 0; i < num_bars; i++) {
        double val = atof(bar_keys[i]->s_name);
        if (val >= 0.0) {
            temp_schedule[actual_count++] = val;
        }
    }

    for (long i = 0; i < num_caps; i++) {
        double val = atof(cap_keys[i]->s_name);
        if (val >= 0.0) {
            temp_schedule[actual_count++] = val;
        }
    }

    if (actual_count > 0) {
        qsort(temp_schedule, actual_count, sizeof(double), compare_doubles);

        long unique_count = 1;
        for (long i = 1; i < actual_count; i++) {
            if (fabs(temp_schedule[i] - temp_schedule[i-1]) > 0.0001) {
                temp_schedule[unique_count++] = temp_schedule[i];
            }
        }
        actual_count = unique_count;
    }

    critical_enter(x->lock);
    if (actual_count > tr->schedule_size) {
        if (tr->schedule) sysmem_freeptr(tr->schedule);
        tr->schedule_size = actual_count + 16;
        tr->schedule = (double *)sysmem_newptr(tr->schedule_size * sizeof(double));
    }
    if (tr->schedule) {
        memcpy(tr->schedule, temp_schedule, actual_count * sizeof(double));
    }
    tr->schedule_count = actual_count;
    tr->next_schedule_idx = 0; // Force re-scan of new schedule

    critical_exit(x->lock);

    sysmem_freeptr(temp_schedule);
    if (bar_keys) dictionary_freekeys(track_dict, num_bars, bar_keys);
    if (cap_keys) sysmem_freeptr(cap_keys);
    if (dict) dictobj_release(dict);
}

void weaver_update_all_schedules(t_weaver *x) {
    if (!x->track_states) return;
    long num_items = 0;
    t_symbol **keys = NULL;
    hashtab_getkeys(x->track_states, &num_items, &keys);
    for (long i = 0; i < num_items; i++) {
        t_weaver_track *tr = NULL;
        hashtab_lookup(x->track_states, keys[i], (t_object **)&tr);
        if (tr) {
            weaver_track_update_schedule(x, tr, atol(keys[i]->s_name));
        }
    }
    if (keys) sysmem_freeptr(keys);
}

t_weaver_track *weaver_get_track_state(t_weaver *x, t_atom_long track_id) {
    t_weaver_track *tr = NULL;
    char tstr[64];
    snprintf(tstr, 64, "%lld", (long long)track_id);
    t_symbol *s_track = gensym(tstr);

    if (hashtab_lookup(x->track_states, s_track, (t_object **)&tr) != MAX_ERR_NONE) {
        tr = (t_weaver_track *)sysmem_newptr(sizeof(t_weaver_track));
        if (tr) {
            crossfade_init(&tr->xf, sys_getsr(), x->low_ms, x->high_ms);
            tr->palette[0] = gensym("-");
            tr->offset[0] = -1.0;
            tr->palette[1] = gensym("-");
            tr->offset[1] = -1.0;
            tr->control = 0.0;
            tr->busy = 0;
            tr->src_refs[0] = buffer_ref_new((t_object *)x, _sym_nothing);
            tr->src_refs[1] = buffer_ref_new((t_object *)x, _sym_nothing);

            char bufname[256];
            snprintf(bufname, 256, "%s.%lld", x->poly_prefix->s_name, (long long)track_id);
            tr->dest_ref = buffer_ref_new((t_object *)x, gensym(bufname));

            tr->dest_found = 0;
            tr->dest_warn_sent = 0;
            tr->src_found[0] = 0;
            tr->src_found[1] = 0;
            tr->src_error_sent[0] = 0;
            tr->src_error_sent[1] = 0;
            tr->track_length = 1000.0;
            tr->last_track_scan = -1.0;
            tr->last_visualize_ms = -1000.0;
            tr->schedule = NULL;
            tr->schedule_count = 0;
            tr->schedule_size = 0;
            tr->silence_caps = hashtab_new(0);
            tr->next_schedule_idx = 0;

            hashtab_store(x->track_states, s_track, (t_object *)tr);
            weaver_track_update_schedule(x, tr, (long)track_id);
        }
    }
    return tr;
}

void weaver_update_track_cache(t_weaver *x) {
    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        x->track_cache_count = 0;
        long limit = (x->max_tracks > 256) ? 256 : x->max_tracks;
        for (long i = 1; i <= limit; i++) {
            x->track_cache[x->track_cache_count++] = weaver_get_track_state(x, (t_atom_long)i);
        }
        critical_exit(x->lock);
    }
}


void weaver_clear_track_states(t_weaver *x) {
    if (!x->track_states) return;
    long num_items = 0;
    t_symbol **keys = NULL;
    hashtab_getkeys(x->track_states, &num_items, &keys);
    for (long i = 0; i < num_items; i++) {
        t_weaver_track *tr = NULL;
        hashtab_lookup(x->track_states, keys[i], (t_object **)&tr);
        if (tr) {
            if (tr->src_refs[0]) object_free(tr->src_refs[0]);
            if (tr->src_refs[1]) object_free(tr->src_refs[1]);
            if (tr->dest_ref) object_free(tr->dest_ref);
            if (tr->schedule) sysmem_freeptr(tr->schedule);
            if (tr->silence_caps) {
                hashtab_clear(tr->silence_caps);
                object_free(tr->silence_caps);
            }
            sysmem_freeptr(tr);
        }
    }
    if (keys) sysmem_freeptr(keys);
    hashtab_clear(x->track_states);
}

void weaver_schedule_silence(t_weaver *x, t_weaver_track *tr, long track_id, double ms) {
    if (!tr || !tr->silence_caps) return;
    char ms_str[64];
    snprintf(ms_str, 64, "%ld", (long)ms);
    t_symbol *s_ms = gensym(ms_str);
    hashtab_store(tr->silence_caps, s_ms, (t_object*)1);
    weaver_track_update_schedule(x, tr, track_id);
}

// Helper function to send verbose log messages with prefix
void weaver_log(t_weaver *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "weaver~", fmt, args);
    va_end(args);
}

void weaver_check_attachments(t_weaver *x) {
    // Dictionary check
    if (x->audio_dict_name != _sym_nothing) {
        t_dictionary *d = dictobj_findregistered_retain(x->audio_dict_name);
        if (d) {
            if (!x->dict_found) {
                weaver_log(x, "successfully found dictionary '%s'", x->audio_dict_name->s_name);
                x->dict_found = 1;
                x->dict_error_sent = 0;
            }
            dictobj_release(d);
        } else {
            x->dict_found = 0;
            if (!x->dict_error_sent) {
                object_error((t_object *)x, "dictionary '%s' not found", x->audio_dict_name->s_name);
                x->dict_error_sent = 1;
            }
            // Kick for dictionary name symbol
            t_symbol *tmp = x->audio_dict_name;
            x->audio_dict_name = _sym_nothing;
            x->audio_dict_name = tmp;
        }
    }

    // Polybuffer check (via track1_ref)
    if (x->poly_prefix != _sym_nothing) {
        char t1name[256];
        snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
        t_symbol *s_t1name = gensym(t1name);

        t_buffer_obj *b = buffer_ref_getobject(x->track1_ref);
        if (!b) {
            // Kick
            buffer_ref_set(x->track1_ref, _sym_nothing);
            buffer_ref_set(x->track1_ref, s_t1name);
            b = buffer_ref_getobject(x->track1_ref);
        }

        if (b) {
            if (!x->poly_found) {
                weaver_log(x, "successfully found polybuffer~ '%s' (via %s)", x->poly_prefix->s_name, s_t1name->s_name);
                x->poly_found = 1;
                x->poly_warn_sent = 0;
            }
        } else {
            x->poly_found = 0;
            if (!x->poly_warn_sent) {
                object_warn((t_object *)x, "polybuffer~ '%s' not found (could not find %s)", x->poly_prefix->s_name, s_t1name->s_name);
                x->poly_warn_sent = 1;
            }
        }
    }
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("weaver~", (method)weaver_new, (method)weaver_free, sizeof(t_weaver), 0L, A_GIMME, 0);

    class_addmethod(c, (method)weaver_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)weaver_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)weaver_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)weaver_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)weaver_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "visualize", 0, t_weaver, visualize);
    CLASS_ATTR_STYLE_LABEL(c, "visualize", 0, "onoff", "Enable Visualization");
    CLASS_ATTR_DEFAULT(c, "visualize", 0, "0");

    CLASS_ATTR_LONG(c, "tracks", 0, t_weaver, max_tracks);
    CLASS_ATTR_LABEL(c, "tracks", 0, "Number of Tracks");
    CLASS_ATTR_DEFAULT(c, "tracks", 0, "4");

    CLASS_ATTR_LONG(c, "log", 0, t_weaver, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_DOUBLE(c, "low", 0, t_weaver, low_ms);
    CLASS_ATTR_LABEL(c, "low", 0, "Low Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "low", 0, "22.653");

    CLASS_ATTR_DOUBLE(c, "high", 0, t_weaver, high_ms);
    CLASS_ATTR_LABEL(c, "high", 0, "High Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "high", 0, "4999.0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    weaver_class = c;
}

void *weaver_new(t_symbol *s, long argc, t_atom *argv) {
    t_weaver *x = (t_weaver *)object_alloc(weaver_class);

    if (x) {
        visualize_init();
        x->poly_prefix = _sym_nothing;
        x->log = 0;
        x->visualize = 0;
        x->audio_dict_name = _sym_nothing;
        x->last_scan_val = -1.0;
        x->fifo_head = 0;
        x->fifo_tail = 0;
        x->dict_found = 0;
        x->dict_error_sent = 0;
        x->poly_found = 0;
        x->poly_warn_sent = 0;
        x->bar_found = 0;
        x->bar_error_sent = 0;

        // Argument 1: dictionary name
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->audio_dict_name = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory dictionary name argument");
        }

        // Argument 2: polybuffer name
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->poly_prefix = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory polybuffer~ name argument");
        }

        x->max_tracks = 4;
        x->low_ms = 22.653;
        x->high_ms = 4999.0;

        attr_args_process(x, argc, argv);

        weaver_update_track_cache(x);

        // Create outlets from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->loop_outlet = outlet_new((t_object *)x, NULL);

        if (x->poly_prefix != _sym_nothing) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            x->track1_ref = buffer_ref_new((t_object *)x, gensym(t1name));
        } else {
            x->track1_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        }

        // Initial search for mandatory objects
        weaver_check_attachments(x);

        x->bar_buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        critical_new(&x->lock);

        x->track_states = hashtab_new(0);
        x->cached_bar_len = weaver_get_bar_length(x);

        x->proxy = proxy_new((t_object *)x, 1, &x->proxy_id);

        dsp_setup((t_pxobject *)x, 1);
        x->audio_qelem = qelem_new(x, (method)weaver_audio_qtask);
    }
    return x;
}

void weaver_free(t_weaver *x) {
    dsp_free((t_pxobject *)x);
    visualize_cleanup();
    critical_free(x->lock);
    if (x->audio_qelem) qelem_free(x->audio_qelem);
    if (x->proxy) object_free(x->proxy);

    if (x->bar_buffer_ref) {
        object_free(x->bar_buffer_ref);
    }
    if (x->track1_ref) {
        object_free(x->track1_ref);
    }
    weaver_clear_track_states(x);
    if (x->track_states) object_free(x->track_states);
}

t_max_err weaver_notify(t_weaver *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    if (x->track_states) {
        long num_items = 0;
        t_symbol **keys = NULL;
        hashtab_getkeys(x->track_states, &num_items, &keys);
        for (long i = 0; i < num_items; i++) {
            t_weaver_track *tr = NULL;
            hashtab_lookup(x->track_states, keys[i], (t_object **)&tr);
            if (tr) {
                if (tr->src_refs[0]) buffer_ref_notify(tr->src_refs[0], s, msg, sender, data);
                if (tr->src_refs[1]) buffer_ref_notify(tr->src_refs[1], s, msg, sender, data);
                if (tr->dest_ref) buffer_ref_notify(tr->dest_ref, s, msg, sender, data);
            }
        }
        if (keys) sysmem_freeptr(keys);
    }
    if (x->bar_buffer_ref) {
        buffer_ref_notify(x->bar_buffer_ref, s, msg, sender, data);
    }
    if (x->track1_ref) {
        buffer_ref_notify(x->track1_ref, s, msg, sender, data);
    }
    return MAX_ERR_NONE;
}

void weaver_assist(t_weaver *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1 (signal/message): Main time ramp (signal), (symbol) Dictionary Name, tracks (int)"); break;
            case 1: sprintf(s, "Inlet 2 (list): [track_id, length] updates track length and schedule"); break;
        }
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (int): Track ID on Loop"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Logging Outlet"); break;
        }
    }
}

double weaver_get_bar_length(t_weaver *x) {
    double bar_length = 0;
    t_buffer_obj *b = buffer_ref_getobject(x->bar_buffer_ref);
    if (!b) {
        // Kick the buffer reference to force re-binding
        buffer_ref_set(x->bar_buffer_ref, _sym_nothing);
        buffer_ref_set(x->bar_buffer_ref, gensym("bar"));
        b = buffer_ref_getobject(x->bar_buffer_ref);
    }
    if (b) {
        if (!x->bar_found) {
            weaver_log(x, "successfully found buffer~ 'bar'");
            x->bar_found = 1;
            x->bar_error_sent = 0;
        }
        critical_enter(0);
        float *samples = buffer_locksamples(b);
        if (samples) {
            if (buffer_getframecount(b) > 0) {
                bar_length = (double)samples[0];
            }
            buffer_unlocksamples(b);
        }
        critical_exit(0);
    } else {
        x->bar_found = 0;
        if (!x->bar_error_sent) {
            object_error((t_object *)x, "bar buffer~ not found");
            x->bar_error_sent = 1;
        }
    }
    return bar_length;
}


void weaver_process_data(t_weaver *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms, int no_crossfade, int is_bar_0, int skip_trigger) {
    weaver_check_attachments(x);

    double bar_len = weaver_get_bar_length(x);
    if (bar_len <= 0) return;

    // 1. Track State and Destination Buffer Lookup
    t_weaver_track *tr = weaver_get_track_state(x, track);
    if (!tr) return;

    char bufname[256];
    snprintf(bufname, 256, "%s.%lld", x->poly_prefix->s_name, (long long)track);
    t_symbol *s_bufname = gensym(bufname);

    t_buffer_obj *dest_buf = buffer_ref_getobject(tr->dest_ref);
    if (!dest_buf) {
        // Kick
        buffer_ref_set(tr->dest_ref, _sym_nothing);
        buffer_ref_set(tr->dest_ref, s_bufname);
        dest_buf = buffer_ref_getobject(tr->dest_ref);
    }

    if (dest_buf) {
        if (!tr->dest_found) {
            weaver_log(x, "Track %lld: successfully found destination buffer '%s'", track, s_bufname->s_name);
            tr->dest_found = 1;
            tr->dest_warn_sent = 0;
        }
    } else {
        tr->dest_found = 0;
        if (!tr->dest_warn_sent) {
            object_warn((t_object *)x, "Track %lld: destination buffer '%s' not found", track, s_bufname->s_name);
            tr->dest_warn_sent = 1;
        }
        return;
    }

    double sr_dest = buffer_getsamplerate(dest_buf);
    if (sr_dest <= 0) sr_dest = sys_getsr();
    if (sr_dest <= 0) sr_dest = 44100.0;

    long n_frames_dest = buffer_getframecount(dest_buf);
    long n_chans_dest = buffer_getchannelcount(dest_buf);
    long start_frame_abs = (long)round(bar_ms * sr_dest / 1000.0);
    long n_frames_to_weave = (long)round(bar_len * sr_dest / 1000.0);

    // 2. Crossfade Trigger
    crossfade_update_params(&tr->xf, sr_dest, x->low_ms, x->high_ms);

    if (!skip_trigger) {
        int active = (int)round(tr->control);
        int change = (palette != tr->palette[active] || offset_ms != tr->offset[active]);

        if (is_bar_0) {
            if (no_crossfade) {
                // Main song ramp loop: jump immediately
                tr->palette[active] = palette;
                tr->offset[active] = offset_ms;
                tr->src_found[active] = 0;
                tr->src_error_sent[active] = 0;
                tr->control = (double)active;

                // Force ramps to solid states (Slot 0 ON if active==0, Slot 1 ON if active==1)
                tr->xf.ramp1.toggle = (active == 0) ? 0.0 : 1.0;
                tr->xf.ramp1.go = (double)tr->xf.elapsed - 1000000.0;
                tr->xf.ramp2.toggle = (active == 1) ? 0.0 : 1.0;
                tr->xf.ramp2.go = (double)tr->xf.elapsed - 1000000.0;
                tr->xf.last_control = tr->control;
                tr->xf.direction = 0.0;
                tr->busy = 0;
                weaver_log(x, "Track %lld: Main song ramp loop jump to Bar 0 (%s@%.2f)", track, palette->s_name, offset_ms);
            } else if (change || !tr->busy) {
                // Track loop: crossfade
                int other = 1 - active;
                tr->palette[other] = palette;
                tr->offset[other] = offset_ms;
                tr->src_found[other] = 0;
                tr->src_error_sent[other] = 0;
                tr->control = (double)other;
                tr->xf.direction = tr->control - tr->xf.last_control;
                weaver_log(x, "Track %lld: Track loop crossfade to Bar 0 (%s@%.2f)", track, palette->s_name, offset_ms);
                if (x->visualize) {
                    char l_msg[256];
                    snprintf(l_msg, sizeof(l_msg), "{\"track\": %lld, \"ms\": %.2f, \"label\": \"%s@%.0f\", \"f2\": %.1f}",
                             (long long)track, bar_ms, palette->s_name, offset_ms, (double)active);
                    visualize(l_msg);
                }
            }
        } else {
            if (change) {
                int other = 1 - active;
                tr->palette[other] = palette;
                tr->offset[other] = offset_ms;
                tr->src_found[other] = 0;
                tr->src_error_sent[other] = 0;
                tr->control = (double)other;
                tr->xf.direction = tr->control - tr->xf.last_control;
                weaver_log(x, "Track %lld: starting crossfade at %.2f ms to %s@%.2f", track, bar_ms, palette->s_name, offset_ms);
                if (x->visualize) {
                    char l_msg[256];
                    snprintf(l_msg, sizeof(l_msg), "{\"track\": %lld, \"ms\": %.2f, \"label\": \"%s@%.0f\", \"f2\": %.1f}",
                             (long long)track, bar_ms, palette->s_name, offset_ms, (double)active);
                    visualize(l_msg);
                }
            }
        }
    }

    critical_enter(0);
    float *samples_dest = NULL;
    int retries_dest = 0;
    for (retries_dest = 0; retries_dest < 10; retries_dest++) {
        samples_dest = buffer_locksamples(dest_buf);
        if (samples_dest) break;
        systhread_sleep(1);
    }
    if (!samples_dest) {
        critical_exit(0);
        weaver_log(x, "Error: could not lock destination buffer %s", s_bufname->s_name);
        return;
    }

    // Source buffers setup using cached refs
    t_buffer_obj *src_buf[2] = {NULL, NULL};
    float *samples_src[2] = {NULL, NULL};
    long n_frames_src[2] = {0, 0};
    long n_chans_src[2] = {0, 0};
    double sr_src[2] = {0, 0};

    for (int i = 0; i < 2; i++) {
        if (tr->palette[i] == gensym("-") || tr->palette[i] == _sym_nothing) {
            continue;
        }
        buffer_ref_set(tr->src_refs[i], tr->palette[i]);
        src_buf[i] = buffer_ref_getobject(tr->src_refs[i]);
        if (!src_buf[i]) {
            // Kick
            buffer_ref_set(tr->src_refs[i], _sym_nothing);
            buffer_ref_set(tr->src_refs[i], tr->palette[i]);
            src_buf[i] = buffer_ref_getobject(tr->src_refs[i]);
        }

        if (src_buf[i]) {
            if (!tr->src_found[i]) {
                weaver_log(x, "Track %lld: successfully found source buffer '%s'", track, tr->palette[i]->s_name);
                tr->src_found[i] = 1;
                tr->src_error_sent[i] = 0;
            }
            sr_src[i] = buffer_getsamplerate(src_buf[i]);
            if (sr_src[i] <= 0) sr_src[i] = 44100.0;
            n_frames_src[i] = buffer_getframecount(src_buf[i]);
            n_chans_src[i] = buffer_getchannelcount(src_buf[i]);

            for (int retry = 0; retry < 10; retry++) {
                samples_src[i] = buffer_locksamples(src_buf[i]);
                if (samples_src[i]) break;
                systhread_sleep(1);
            }
        } else {
            tr->src_found[i] = 0;
            if (!tr->src_error_sent[i]) {
                object_error((t_object *)x, "Track %lld: source buffer '%s' not found", track, tr->palette[i]->s_name);
                tr->src_error_sent[i] = 1;
            }
        }
    }

    // Weave loop with crossfade and nearest-neighbor sample rate conversion
    for (long i_frame = 0; i_frame < n_frames_to_weave; i_frame++) {
        long f_abs = start_frame_abs + i_frame;
        long f = f_abs % n_frames_dest;
        if (f < 0) f += n_frames_dest;

        double current_ms = (double)f_abs * 1000.0 / sr_dest;
        double track_ramp_ms = fmod(current_ms, tr->track_length);
        double max_abs[2] = {0.0, 0.0};
        long f_src[2] = {-1, -1};

        for (int i = 0; i < 2; i++) {
            if (samples_src[i]) {
                double src_ms = tr->offset[i] + track_ramp_ms;
                f_src[i] = (long)round(src_ms * sr_src[i] / 1000.0);
                if (f_src[i] >= 0 && f_src[i] < n_frames_src[i]) {
                    for (long c = 0; c < n_chans_src[i]; c++) {
                        double a = fabs((double)samples_src[i][f_src[i] * n_chans_src[i] + c]);
                        if (a > max_abs[i]) max_abs[i] = a;
                    }
                }
            }
        }

        double f1, f2;
        // ramp_process returns signal * fade, and updates internal state
        ramp_process(&tr->xf.ramp1, max_abs[0], tr->xf.direction, tr->xf.elapsed, tr->xf.samplerate, tr->xf.low_ms, tr->xf.high_ms, &f1);
        ramp_process(&tr->xf.ramp2, max_abs[1], tr->xf.direction * -1.0, tr->xf.elapsed, tr->xf.samplerate, tr->xf.low_ms, tr->xf.high_ms, &f2);

        // Movement is only non-zero for the first sample to trigger the ramps
        tr->xf.direction = 0.0;

        int r1_done = (tr->xf.ramp1.toggle > 0.5) ? (f1 <= 0.0) : (f1 >= 1.0);
        int r2_done = (tr->xf.ramp2.toggle > 0.5) ? (f2 <= 0.0) : (f2 >= 1.0);
        int finished = r1_done && r2_done;
        tr->xf.last_control = tr->control;
        tr->xf.elapsed++;
        tr->busy = !finished;

        // Apply fades to all destination channels
        for (long c = 0; c < n_chans_dest; c++) {
            double mix1 = 0.0;
            double mix2 = 0.0;
            if (samples_src[0] && f_src[0] >= 0 && f_src[0] < n_frames_src[0] && c < n_chans_src[0]) {
                mix1 = (double)samples_src[0][f_src[0] * n_chans_src[0] + c] * f1;
            }
            if (samples_src[1] && f_src[1] >= 0 && f_src[1] < n_frames_src[1] && c < n_chans_src[1]) {
                mix2 = (double)samples_src[1][f_src[1] * n_chans_src[1] + c] * f2;
            }
            samples_dest[f * n_chans_dest + c] = (float)(mix1 + mix2);
        }

        if (x->visualize && (current_ms >= tr->last_visualize_ms + 100.0 || current_ms < tr->last_visualize_ms)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"track\": %lld, \"ms\": %.2f, \"f1\": %.4f, \"f2\": %.4f}",
                     (long long)track, current_ms, f1, f2);
            visualize(msg);
            tr->last_visualize_ms = current_ms;
        }
    }

    // Cleanup
    for (int i = 0; i < 2; i++) {
        if (src_buf[i] && samples_src[i]) {
            buffer_unlocksamples(src_buf[i]);
        }
    }
    buffer_unlocksamples(dest_buf);
    buffer_setdirty(dest_buf);
    critical_exit(0);
}


void weaver_list(t_weaver *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        if (argc >= 2) {
            long track_id = atom_getlong(argv);
            double length = atom_getfloat(argv + 1);
            if (track_id > 0) {
                t_weaver_track *tr = weaver_get_track_state(x, track_id);
                if (tr) {
                    critical_enter(x->lock);
                    tr->track_length = length;
                    critical_exit(x->lock);
                    weaver_log(x, "Track %ld length manually updated to %.2f ms", track_id, length);
                    weaver_track_update_schedule(x, tr, track_id);
                }
            }
        }
    }
}

void weaver_anything(t_weaver *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) != 0) return;

    if (s == gensym("tracks") && argc > 0) {
        x->max_tracks = atom_getlong(argv);
        weaver_update_track_cache(x);
        weaver_update_all_schedules(x);
    } else if (s != x->audio_dict_name) {
        // Inlet 0: Transcript Dictionary Reference
        x->audio_dict_name = s;
        x->dict_found = 0;
        x->dict_error_sent = 0;
        weaver_update_all_schedules(x);
    }
}

void weaver_dsp64(t_weaver *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    if (count[0]) {
        weaver_log(x, "DSP ON: tracking signal ramp");
        weaver_update_track_cache(x);
        dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)weaver_perform64, 0, NULL);
    }
}

void weaver_perform64(t_weaver *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];
    double last_scan = x->last_scan_val;

    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        for (int i = 0; i < sampleframes; i++) {
            double current_scan = in[i]; // Lookahead removed as per project memory

            for (long t = 0; t < x->track_cache_count; t++) {
                t_weaver_track *tr = x->track_cache[t];
                if (!tr) continue;

                double tr_scan = fmod(current_scan, tr->track_length);

                if (tr->last_track_scan == -1.0) {
                    // Initial scan: find first scheduled bar >= current position
                    tr->next_schedule_idx = 0;
                    while (tr->next_schedule_idx < tr->schedule_count && tr->schedule[tr->next_schedule_idx] < tr_scan) {
                        tr->next_schedule_idx++;
                    }
                } else {
                    int main_looped = (current_scan < last_scan);
                    int track_looped = (tr_scan < tr->last_track_scan);

                    if (main_looped || track_looped) {
                        // Notify loop
                        int nt_loop = (x->fifo_tail + 1) % 4096;
                        if (nt_loop != x->fifo_head) {
                            x->hit_bars[x->fifo_tail].type = TYPE_LOOP;
                            x->hit_bars[x->fifo_tail].track_id = t + 1;
                            x->hit_bars[x->fifo_tail].no_crossfade = main_looped;
                            x->fifo_tail = nt_loop;
                        }

                        double prev_cycle_base = floor(last_scan / tr->track_length) * tr->track_length;
                        double curr_cycle_base = floor(current_scan / tr->track_length) * tr->track_length;

                        // Check remaining bars in old cycle
                        while (tr->next_schedule_idx < tr->schedule_count) {
                            double s = tr->schedule[tr->next_schedule_idx];
                            int nt = (x->fifo_tail + 1) % 4096;
                            if (nt != x->fifo_head) {
                                char bstr[64];
                                snprintf(bstr, 64, "%ld", (long)s);
                                x->hit_bars[x->fifo_tail].bar.sym = gensym(bstr);
                                x->hit_bars[x->fifo_tail].bar.value = prev_cycle_base + s;
                                x->hit_bars[x->fifo_tail].type = TYPE_DATA;
                                x->hit_bars[x->fifo_tail].track_id = t + 1;
                                x->hit_bars[x->fifo_tail].no_crossfade = main_looped;
                                x->fifo_tail = nt;
                            }
                            tr->next_schedule_idx++;
                        }

                        // Reset to start of schedule for new cycle
                        tr->next_schedule_idx = 0;
                        while (tr->next_schedule_idx < tr->schedule_count && tr->schedule[tr->next_schedule_idx] <= tr_scan) {
                            double s = tr->schedule[tr->next_schedule_idx];
                            int nt = (x->fifo_tail + 1) % 4096;
                            if (nt != x->fifo_head) {
                                char bstr[64];
                                snprintf(bstr, 64, "%ld", (long)s);
                                x->hit_bars[x->fifo_tail].bar.sym = gensym(bstr);
                                x->hit_bars[x->fifo_tail].bar.value = curr_cycle_base + s;
                                x->hit_bars[x->fifo_tail].type = TYPE_DATA;
                                x->hit_bars[x->fifo_tail].track_id = t + 1;
                                x->hit_bars[x->fifo_tail].no_crossfade = main_looped;
                                x->fifo_tail = nt;
                            }
                            tr->next_schedule_idx++;
                        }
                    } else {
                        // Normal progression
                        double cycle_base = floor(current_scan / tr->track_length) * tr->track_length;
                        while (tr->next_schedule_idx < tr->schedule_count && tr->schedule[tr->next_schedule_idx] <= tr_scan) {
                            double s = tr->schedule[tr->next_schedule_idx];
                            if (s > tr->last_track_scan) {
                                int nt = (x->fifo_tail + 1) % 4096;
                                if (nt != x->fifo_head) {
                                    char bstr[64];
                                    snprintf(bstr, 64, "%ld", (long)s);
                                    x->hit_bars[x->fifo_tail].bar.sym = gensym(bstr);
                                    x->hit_bars[x->fifo_tail].bar.value = cycle_base + s;
                                    x->hit_bars[x->fifo_tail].type = TYPE_DATA;
                                    x->hit_bars[x->fifo_tail].track_id = t + 1;
                                    x->hit_bars[x->fifo_tail].no_crossfade = 0;
                                    x->fifo_tail = nt;
                                }
                            }
                            tr->next_schedule_idx++;
                        }
                    }
                }
                tr->last_track_scan = tr_scan;
            }
            last_scan = current_scan;
        }

        qelem_set(x->audio_qelem);
        x->last_scan_val = last_scan;
        critical_exit(x->lock);
    }
}

void weaver_audio_qtask(t_weaver *x) {
    weaver_check_attachments(x);
    x->cached_bar_len = weaver_get_bar_length(x);
    double bar_len = x->cached_bar_len;
    int clear_sent = 0;

    t_hashtab *tracks_to_update = hashtab_new(0);

    while (x->fifo_head != x->fifo_tail) {
        t_fifo_entry hit_entry = x->hit_bars[x->fifo_head];
        x->fifo_head = (x->fifo_head + 1) % 4096;

        long target_track = hit_entry.track_id;
        int no_crossfade = hit_entry.no_crossfade;
        t_weaver_track *tr = NULL;
        if (target_track > 0 && target_track <= x->track_cache_count) {
            tr = x->track_cache[target_track - 1];
        }

        if (hit_entry.type == TYPE_LOOP) {
            outlet_int(x->loop_outlet, (t_atom_long)hit_entry.track_id);
            if (x->visualize && hit_entry.no_crossfade && !clear_sent) {
                visualize("{\"clear\": 1}");
                clear_sent = 1;
            }
            continue;
        }

        t_bar_cache hit = hit_entry.bar;

        // DATA Hit logic
        t_symbol *bar_key = hit.sym;
        if (!bar_key) {
            char bstr[64];
            snprintf(bstr, 64, "%ld", (long)hit.value);
            bar_key = gensym(bstr);
        }
        int is_bar_0 = (bar_key == gensym("0"));

        if (tr && tr->busy && !no_crossfade) {
            int active = (int)round(tr->control);
            weaver_process_data(x, tr->palette[active], target_track, hit.value, tr->offset[active], no_crossfade, 0, 1);
            continue;
        }

        t_dictionary *dict = dictobj_findregistered_retain(x->audio_dict_name);
        char tstr[64];
        snprintf(tstr, 64, "%ld", target_track);
        t_symbol *track_sym = gensym(tstr);
        t_dictionary *track_dict = NULL;

        int found_in_dict = 0;

        if (dict && dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict) == MAX_ERR_NONE && track_dict) {
            t_dictionary *bar_dict = NULL;
            if (dictionary_getdictionary(track_dict, bar_key, (t_object **)&bar_dict) == MAX_ERR_NONE && bar_dict) {
                found_in_dict = 1;
                t_symbol *palette = _sym_nothing;
                double offset = 0.0;

                t_atomarray *palette_aa = NULL;
                t_atom p_atom;
                if (dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_aa) == MAX_ERR_NONE && palette_aa) {
                    if (atomarray_getindex(palette_aa, 0, &p_atom) == MAX_ERR_NONE) palette = atom_getsym(&p_atom);
                } else if (dictionary_getatom(bar_dict, gensym("palette"), &p_atom) == MAX_ERR_NONE) {
                    palette = atom_getsym(&p_atom);
                }

                t_atomarray *offset_aa = NULL;
                t_atom o_atom;
                if (dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_aa) == MAX_ERR_NONE && offset_aa) {
                    if (atomarray_getindex(offset_aa, 0, &o_atom) == MAX_ERR_NONE) offset = atom_getfloat(&o_atom);
                } else if (dictionary_getatom(bar_dict, gensym("offset"), &o_atom) == MAX_ERR_NONE) {
                    offset = atom_getfloat(&o_atom);
                }

                weaver_process_data(x, palette, target_track, hit.value, offset, no_crossfade, is_bar_0, 0);
            }
        }

        if (!found_in_dict) {
            // Check if it's a silence cap
            if (hashtab_lookup(tr->silence_caps, bar_key, NULL) == MAX_ERR_NONE) {
                weaver_process_data(x, gensym("-"), target_track, hit.value, 0.0, no_crossfade, is_bar_0, 0);
            } else if (is_bar_0) {
                // Bar 0 missing: force silence
                weaver_process_data(x, gensym("-"), target_track, hit.value, 0.0, no_crossfade, is_bar_0, 0);
            }
        }

        // Always remove processed bar from silence caps to prevent growth
        hashtab_delete(tr->silence_caps, bar_key);

        // 2. Silence Cap logic (using current information)
        if (tr && bar_len > 0) {
            double curr_rel = atof(bar_key->s_name);
            double next_rel = fmod(curr_rel + bar_len, tr->track_length);
            char next_rel_str[64];
            snprintf(next_rel_str, 64, "%ld", (long)next_rel);
            t_symbol *next_rel_sym = gensym(next_rel_str);

            int next_exists = 0;
            if (track_dict && dictionary_hasentry(track_dict, next_rel_sym)) {
                next_exists = 1;
            }

            if (!next_exists) {
                // Add to silence_caps but don't update schedule yet
                hashtab_store(tr->silence_caps, next_rel_sym, (t_object*)1);
            }
        }

        // Mark track for schedule update at the end of task
        if (tr) {
            char tstr_key[64];
            snprintf(tstr_key, 64, "%ld", target_track);
            hashtab_store(tracks_to_update, gensym(tstr_key), (t_object*)1);
        }

        if (dict) dictobj_release(dict);
    }

    // Perform deferred schedule updates
    long num_update = 0;
    t_symbol **update_keys = NULL;
    hashtab_getkeys(tracks_to_update, &num_update, &update_keys);
    for (long i = 0; i < num_update; i++) {
        long tid = atol(update_keys[i]->s_name);
        t_weaver_track *tr = weaver_get_track_state(x, tid);
        if (tr) {
            weaver_track_update_schedule(x, tr, tid);
        }
    }
    if (update_keys) sysmem_freeptr(update_keys);
    hashtab_clear(tracks_to_update);
    object_free(tracks_to_update);
}

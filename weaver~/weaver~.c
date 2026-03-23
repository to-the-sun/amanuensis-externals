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
    double rel_time;
    double range_end; // For SWEEP
    long type; // 0 for DATA, 1 for SWEEP
    long track_id;
    int no_crossfade;
} t_fifo_entry;

typedef struct _weaver_track {
    t_crossfade_state xf;
    t_symbol *palette[2];
    double offset[2];
    double dict_offset[2];
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

    // Thread-safe state handover
    t_symbol *pending_palette;
    double pending_offset;
    t_symbol *pending_bar_symbol;
    int pending_no_crossfade;
    int has_pending_data;
    int waiting_for_dict;

    // Visualization state
    double viz_f1;
    double viz_f2;
    t_symbol *viz_palette;
    double viz_offset;
    t_symbol *viz_bar_symbol;
    double viz_ms;
    int viz_dirty;
    int viz_trigger_dirty;
    int dirty_dest;
    double viz_control;
    double viz_track_length;

    int last_busy_logged;
    int viz_busy;
    double last_viz_sent_ms;
} t_weaver_track;

#define MAX_WEAVER_TRACKS 256

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
    t_weaver_track *track_cache[MAX_WEAVER_TRACKS];
    long track_cache_count;
    void *proxy;
    long proxy_id;
    long bar_found;
    long bar_error_sent;
    long dict_found;
    long dict_error_sent;
    long poly_found;
    long poly_warn_sent;

    t_hashtab *track_states;
    double low_ms;
    double high_ms;

    double last_viz_check_ms;
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
void weaver_update_track_metadata(t_weaver *x, t_atom_long track, t_symbol *palette, double bar_ms, double offset_ms, int no_crossfade, t_symbol *bar_symbol);
void weaver_check_attachments(t_weaver *x);
double weaver_get_bar_length(t_weaver *x);
void weaver_dsp64(t_weaver *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void weaver_perform64(t_weaver *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void weaver_audio_qtask(t_weaver *x);
t_weaver_track *weaver_get_track_state(t_weaver *x, t_atom_long track_id);
void weaver_clear_track_states(t_weaver *x);
void weaver_update_track_cache(t_weaver *x);
static t_class *weaver_class;
static t_symbol *_sym_dash;

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
            tr->dict_offset[0] = -1.0;
            tr->palette[1] = gensym("-");
            tr->offset[1] = -1.0;
            tr->dict_offset[1] = -1.0;
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

            // Thread-safe state handover init
            tr->pending_palette = _sym_nothing;
            tr->pending_offset = 0.0;
            tr->pending_bar_symbol = _sym_nothing;
            tr->pending_no_crossfade = 0;
            tr->has_pending_data = 0;
            tr->waiting_for_dict = 0;

            // Visualization state init
            tr->viz_f1 = 0.0;
            tr->viz_f2 = 0.0;
            tr->viz_palette = _sym_nothing;
            tr->viz_offset = 0.0;
            tr->viz_bar_symbol = _sym_nothing;
            tr->viz_ms = 0.0;
            tr->viz_dirty = 0;
            tr->viz_trigger_dirty = 0;
            tr->dirty_dest = 0;
            tr->viz_control = 0.0;
            tr->viz_track_length = 1000.0;
            tr->last_busy_logged = -1;
            tr->viz_busy = 0;
            tr->last_viz_sent_ms = -1000.0;

            hashtab_store(x->track_states, s_track, (t_object *)tr);
        }
    }
    return tr;
}

void weaver_update_track_cache(t_weaver *x) {
    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        x->track_cache_count = 0;
        long limit = (x->max_tracks > MAX_WEAVER_TRACKS) ? MAX_WEAVER_TRACKS : x->max_tracks;
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
            sysmem_freeptr(tr);
        }
    }
    if (keys) sysmem_freeptr(keys);
    hashtab_clear(x->track_states);
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

    // Bar buffer check
    t_buffer_obj *b_bar = buffer_ref_getobject(x->bar_buffer_ref);
    if (!b_bar) {
        // Kick
        buffer_ref_set(x->bar_buffer_ref, _sym_nothing);
        buffer_ref_set(x->bar_buffer_ref, gensym("bar"));
        b_bar = buffer_ref_getobject(x->bar_buffer_ref);
    }

    if (b_bar) {
        if (!x->bar_found) {
            weaver_log(x, "successfully found buffer~ 'bar'");
            x->bar_found = 1;
            x->bar_error_sent = 0;
        }
    } else {
        x->bar_found = 0;
        if (!x->bar_error_sent) {
            object_error((t_object *)x, "bar buffer~ not found");
            x->bar_error_sent = 1;
        }
    }
}

void ext_main(void *r) {
    common_symbols_init();
    _sym_dash = gensym("-");
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
        x->max_tracks = 4;
        x->low_ms = 22.653;
        x->high_ms = 4999.0;
        x->track_cache_count = 0;
        x->last_viz_check_ms = 0;

        // 1. Initialize core structures and sync objects early
        critical_new(&x->lock);
        x->track_states = hashtab_new(0);
        x->bar_buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        // 2. Create outlets (before any logging happens)
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->loop_outlet = outlet_new((t_object *)x, NULL);

        // 3. Process Arguments
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

        // 4. Initialize polybuffer tracking
        if (x->poly_prefix != _sym_nothing) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            x->track1_ref = buffer_ref_new((t_object *)x, gensym(t1name));
        } else {
            x->track1_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        }

        attr_args_process(x, argc, argv);

        // 5. Setup initial state dependent on initialized refs and locks
        weaver_check_attachments(x);
        weaver_update_track_cache(x);

        x->proxy = proxy_new((t_object *)x, 1, &x->proxy_id);

        dsp_setup((t_pxobject *)x, 1);
        x->audio_qelem = qelem_new(x, (method)weaver_audio_qtask);
    }
    return x;
}

void weaver_free(t_weaver *x) {
    dsp_free((t_pxobject *)x);
    visualize_cleanup();
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
    if (x->lock) critical_free(x->lock);
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
            case 1: sprintf(s, "Inlet 2 (list): [track_id, length] updates track length"); break;
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
    if (b) {
        float *samples = buffer_locksamples(b);
        if (samples) {
            if (buffer_getframecount(b) > 0) {
                bar_length = (double)samples[0];
            }
            buffer_unlocksamples(b);
        }
    }
    return bar_length;
}


void weaver_update_track_metadata(t_weaver *x, t_atom_long track, t_symbol *palette, double bar_ms, double offset_ms, int no_crossfade, t_symbol *bar_symbol) {
    t_weaver_track *tr = weaver_get_track_state(x, track);
    if (!tr) return;

    critical_enter(x->lock);
    tr->pending_palette = palette;
    tr->pending_offset = offset_ms;
    tr->pending_bar_symbol = bar_symbol;
    tr->pending_no_crossfade = no_crossfade;
    if (x->visualize) {
        tr->viz_ms = bar_ms; // Trigger timestamp for viz
        tr->viz_control = tr->control;
        tr->viz_track_length = tr->track_length;
    }

    // Pre-bind palette to buffer_ref (slot logic will be applied in DSP thread)
    // We update both refs because we don't know which slot is currently available,
    // but the DSP thread will use the palette symbol from metadata to lock the correct buffer.
    // Actually, it's safer to just set them both if it's a new palette.
    if (palette != _sym_nothing && palette != gensym("-")) {
        buffer_ref_set(tr->src_refs[0], palette);
        buffer_ref_set(tr->src_refs[1], palette);
    }

    tr->has_pending_data = 1;
    critical_exit(x->lock);
}


void weaver_list(t_weaver *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        if (argc >= 2) {
            long track_id = atom_getlong(argv);
            double length = round(atom_getfloat(argv + 1));
            if (track_id > 0) {
                t_weaver_track *tr = weaver_get_track_state(x, track_id);
                if (tr) {
                    critical_enter(x->lock);
                    tr->track_length = length;
                    critical_exit(x->lock);
                    weaver_log(x, "Track %ld length manually updated to %.2f ms", track_id, length);
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
    } else if (s != x->audio_dict_name) {
        // Inlet 0: Transcript Dictionary Reference
        x->audio_dict_name = s;
        x->dict_found = 0;
        x->dict_error_sent = 0;
    }
}

void weaver_dsp64(t_weaver *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    if (count[0]) {
        weaver_log(x, "DSP ON: tracking signal ramp");
        weaver_update_track_cache(x);
        dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)weaver_perform64, 0, NULL);
    }
}

typedef struct {
    float *samples_dest;
    long long n_frames_dest;
    long n_chans_dest;
    double sr_dest;

    float *samples_src[2];
    long long n_frames_src[2];
    long n_chans_src[2];
    double sr_src[2];
    t_buffer_obj *buf_src[2];
    t_buffer_obj *buf_dest;
} t_track_buffers;

void weaver_perform64(t_weaver *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];
    double last_scan = x->last_scan_val;
    double bar_len = round(weaver_get_bar_length(x));

    t_track_buffers tb[MAX_WEAVER_TRACKS];
    memset(tb, 0, sizeof(tb));

    // 1. Handover and Buffer Acquisition
    int has_lock = (critical_tryenter(x->lock) == MAX_ERR_NONE);

    for (long t = 0; t < x->track_cache_count; t++) {
        t_weaver_track *tr = x->track_cache[t];
        if (!tr) continue;

        if (has_lock && tr->has_pending_data) {
            int active = (int)round(tr->control);
            int change = (tr->pending_palette != tr->palette[active] || tr->pending_offset != tr->dict_offset[active]);

            if (tr->pending_no_crossfade) {
                tr->palette[active] = tr->pending_palette;
                tr->dict_offset[active] = tr->pending_offset;
                tr->offset[active] = tr->pending_offset - tr->viz_ms;
                tr->xf.ramp1.toggle = (active == 0) ? 0.0 : 1.0;
                tr->xf.ramp1.go = (double)tr->xf.elapsed - 1000000.0;
                tr->xf.ramp2.toggle = (active == 1) ? 0.0 : 1.0;
                tr->xf.ramp2.go = (double)tr->xf.elapsed - 1000000.0;
                tr->xf.direction = 0.0;
                tr->busy = 0;
            } else if (change) {
                int other = 1 - active;
                tr->palette[other] = tr->pending_palette;
                tr->dict_offset[other] = tr->pending_offset;
                tr->offset[other] = tr->pending_offset - tr->viz_ms;
                tr->control = (double)other;
                tr->xf.direction = tr->control - tr->xf.last_control;
            }
            if (x->visualize) {
                tr->viz_palette = tr->pending_palette;
                tr->viz_offset = tr->pending_offset;
                tr->viz_bar_symbol = tr->pending_bar_symbol;
                tr->viz_trigger_dirty = 1;
            }
            tr->has_pending_data = 0;
            tr->waiting_for_dict = 0;
        }

        // 2. Lock Buffers
        t_buffer_obj *dest_buf = buffer_ref_getobject(tr->dest_ref);
        if (dest_buf) {
            tb[t].samples_dest = buffer_locksamples(dest_buf);
            if (tb[t].samples_dest) {
                tb[t].buf_dest = dest_buf;
                tb[t].n_frames_dest = buffer_getframecount(dest_buf);
                tb[t].n_chans_dest = buffer_getchannelcount(dest_buf);
                tb[t].sr_dest = buffer_getsamplerate(dest_buf);
                if (tb[t].sr_dest <= 0) tb[t].sr_dest = sys_getsr();
            }
        }

        for (int j = 0; j < 2; j++) {
            if (tr->palette[j] != _sym_nothing && tr->palette[j] != _sym_dash) {
                t_buffer_obj *src_buf = buffer_ref_getobject(tr->src_refs[j]);
                if (src_buf) {
                    tb[t].samples_src[j] = buffer_locksamples(src_buf);
                    if (tb[t].samples_src[j]) {
                        tb[t].buf_src[j] = src_buf;
                        tb[t].n_frames_src[j] = buffer_getframecount(src_buf);
                        tb[t].n_chans_src[j] = buffer_getchannelcount(src_buf);
                        tb[t].sr_src[j] = buffer_getsamplerate(src_buf);
                        if (tb[t].sr_src[j] <= 0) tb[t].sr_src[j] = sys_getsr();
                    }
                }
            }
        }
        crossfade_update_params(&tr->xf, tb[t].sr_dest > 0 ? tb[t].sr_dest : sys_getsr(), x->low_ms, x->high_ms);
    }

    if (has_lock) critical_exit(x->lock);

    // 3. Sample Loop (Unlocked)
    for (int i = 0; i < sampleframes; i++) {
            double current_scan = in[i];
            int main_looped = (current_scan < last_scan);

            for (long t = 0; t < x->track_cache_count; t++) {
                t_weaver_track *tr = x->track_cache[t];
                if (!tr) continue;

                // 2. Continuous Bar Hit Detection (Outside samples_dest check)
                double tr_scan = fmod(current_scan, tr->track_length);
                long r_scan = (long)floor(tr_scan);
                long r_last = (long)floor(tr->last_track_scan);
                int track_looped = (r_scan < r_last);

                if (tr->last_track_scan != -1.0) {
                    if (main_looped || track_looped) {
                        int nt_loop = (x->fifo_tail + 1) % 4096;
                        if (nt_loop != x->fifo_head) {
                            x->hit_bars[x->fifo_tail].type = TYPE_LOOP;
                            x->hit_bars[x->fifo_tail].track_id = t + 1;
                            x->hit_bars[x->fifo_tail].no_crossfade = main_looped;
                            x->fifo_tail = nt_loop;
                        }
                    }

                    if ((!tr->busy || main_looped) && !tr->waiting_for_dict && r_scan != r_last && bar_len > 0) {
                        long long start = (track_looped || main_looped) ? 0 : r_last + 1;
                        long long end = r_scan;
                        long long latest_j = (end / (long long)bar_len) * (long long)bar_len;

                        if (latest_j >= start) {
                            int nt = (x->fifo_tail + 1) % 4096;
                            if (nt != x->fifo_head) {
                                x->hit_bars[x->fifo_tail].bar.sym = NULL;
                                x->hit_bars[x->fifo_tail].rel_time = (double)latest_j;
                                x->hit_bars[x->fifo_tail].bar.value = current_scan; // Current ramp
                                x->hit_bars[x->fifo_tail].type = TYPE_DATA;
                                x->hit_bars[x->fifo_tail].track_id = t + 1;
                                x->hit_bars[x->fifo_tail].no_crossfade = main_looped;
                                x->fifo_tail = nt;
                                tr->waiting_for_dict = 1;
                                tr->busy = 1;
                            }
                        }
                    }
                } else if (bar_len > 0) {
                    // Initial Bar Trigger
                    double initial_bar = floor(tr_scan / bar_len) * bar_len;
                    int nt_init = (x->fifo_tail + 1) % 4096;
                    if (nt_init != x->fifo_head) {
                        x->hit_bars[x->fifo_tail].bar.sym = NULL;
                        x->hit_bars[x->fifo_tail].rel_time = initial_bar;
                        x->hit_bars[x->fifo_tail].bar.value = current_scan;
                        x->hit_bars[x->fifo_tail].type = TYPE_DATA;
                        x->hit_bars[x->fifo_tail].track_id = t + 1;
                        x->hit_bars[x->fifo_tail].no_crossfade = 0;
                        x->fifo_tail = nt_init;
                        tr->waiting_for_dict = 1;
                        tr->busy = 1;
                    }
                }

                // 3. One-sample audio weaving (Inside samples_dest check)
                if (!tb[t].samples_dest) {
                    tr->last_track_scan = tr_scan;
                    continue;
                }

                long long f_dest = (long long)round(current_scan * tb[t].sr_dest / 1000.0) % tb[t].n_frames_dest;
                if (f_dest < 0) f_dest += tb[t].n_frames_dest;

                double max_abs[2] = {0.0, 0.0};
                long long f_src[2] = {-1, -1};

                for (int j = 0; j < 2; j++) {
                    if (tb[t].samples_src[j]) {
                        double src_ms = tr->offset[j] + current_scan;
                        f_src[j] = (long long)round(src_ms * tb[t].sr_src[j] / 1000.0);
                        if (f_src[j] >= 0 && f_src[j] < tb[t].n_frames_src[j]) {
                            for (long c = 0; c < tb[t].n_chans_src[j]; c++) {
                                double a = fabs((double)tb[t].samples_src[j][f_src[j] * tb[t].n_chans_src[j] + c]);
                                if (a > max_abs[j]) max_abs[j] = a;
                            }
                        }
                    }
                }

                double f1, f2;
                ramp_process(&tr->xf.ramp1, max_abs[0], tr->xf.direction, tr->xf.elapsed, tr->xf.samplerate, tr->xf.low_ms, tr->xf.high_ms, &f1);
                ramp_process(&tr->xf.ramp2, max_abs[1], tr->xf.direction * -1.0, tr->xf.elapsed, tr->xf.samplerate, tr->xf.low_ms, tr->xf.high_ms, &f2);
                tr->xf.direction = 0.0;

                int r1_done = (tr->xf.ramp1.toggle > 0.5) ? (f1 <= 0.0) : (f1 >= 1.0);
                int r2_done = (tr->xf.ramp2.toggle > 0.5) ? (f2 <= 0.0) : (f2 >= 1.0);
                if (r1_done && r2_done && !tr->waiting_for_dict) tr->busy = 0;

                tr->xf.last_control = tr->control;
                tr->xf.elapsed = (long long)round(current_scan * tb[t].sr_dest / 1000.0);

                for (long c = 0; c < tb[t].n_chans_dest; c++) {
                    double mix1 = 0.0;
                    double mix2 = 0.0;
                    if (tb[t].samples_src[0] && f_src[0] >= 0 && f_src[0] < tb[t].n_frames_src[0] && c < tb[t].n_chans_src[0]) {
                        mix1 = (double)tb[t].samples_src[0][f_src[0] * tb[t].n_chans_src[0] + c] * f1;
                    }
                    if (tb[t].samples_src[1] && f_src[1] >= 0 && f_src[1] < tb[t].n_frames_src[1] && c < tb[t].n_chans_src[1]) {
                        mix2 = (double)tb[t].samples_src[1][f_src[1] * tb[t].n_chans_src[1] + c] * f2;
                    }
                    tb[t].samples_dest[f_dest * tb[t].n_chans_dest + c] = (float)(mix1 + mix2);
                }
                tr->dirty_dest = 1;

                if (x->visualize) {
                    tr->viz_f1 = f1;
                    tr->viz_f2 = f2;
                    tr->viz_busy = tr->busy;

                    if (current_scan >= tr->last_viz_sent_ms + 333.33 || current_scan < tr->last_viz_sent_ms || tr->viz_busy != tr->busy) {
                        tr->viz_dirty = 1;
                        tr->last_viz_sent_ms = current_scan;
                    }
                }

                tr->last_track_scan = tr_scan;
            }
            last_scan = current_scan;
        }

    // 4. Unlock Phase
    for (long t = 0; t < x->track_cache_count; t++) {
        if (tb[t].buf_dest && tb[t].samples_dest) {
            buffer_unlocksamples(tb[t].buf_dest);
        }
        for (int j = 0; j < 2; j++) {
            if (tb[t].buf_src[j] && tb[t].samples_src[j]) {
                buffer_unlocksamples(tb[t].buf_src[j]);
            }
        }
    }

    x->last_scan_val = (sampleframes > 0) ? in[sampleframes - 1] : last_scan;
    qelem_set(x->audio_qelem);
}

void weaver_audio_qtask(t_weaver *x) {
    weaver_check_attachments(x);
    int clear_sent = 0;

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
            snprintf(bstr, 64, "%ld", (long)round(hit_entry.rel_time));
            bar_key = gensym(bstr);
        }

        t_dictionary *dict = dictobj_findregistered_retain(x->audio_dict_name);
        if (dict) {
            char tstr[64];
            snprintf(tstr, 64, "%ld", target_track);
            t_symbol *track_sym = gensym(tstr);
            t_dictionary *track_dict = NULL;

            int found_in_dict = 0;

            if (dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict) == MAX_ERR_NONE && track_dict) {
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

                    weaver_log(x, "Track %lld: bar %s found in dictionary (palette: %s, offset: %.2f)", (long long)target_track, bar_key->s_name, palette->s_name, offset);
                    weaver_update_track_metadata(x, target_track, palette, hit.value, offset, no_crossfade, bar_key);
                }
            }

            if (!found_in_dict) {
                // Trigger silence if bar missing from dictionary
                weaver_update_track_metadata(x, target_track, gensym("-"), hit.value, 0.0, no_crossfade, bar_key);
            }

            dictobj_release(dict);
        } else {
            // Even if dictionary is missing, we must trigger something (e.g. silence) to progress
            weaver_update_track_metadata(x, target_track, gensym("-"), hit.value, 0.0, no_crossfade, bar_key);
        }
    }

    // 1. Post-DSP Buffer and Busy Management
    for (long t = 0; t < x->track_cache_count; t++) {
        t_weaver_track *tr = x->track_cache[t];
        if (tr) {
            if (tr->dirty_dest) {
                t_buffer_obj *b = buffer_ref_getobject(tr->dest_ref);
                if (b) buffer_setdirty(b);
                tr->dirty_dest = 0;
            }

            // Busy State Logging
            if (tr->busy != tr->last_busy_logged) {
                weaver_log(x, "track %ld busy: %d", t + 1, tr->busy);
                tr->last_busy_logged = tr->busy;
            }
        }
    }

    // 2. Visualization Polling
    if (x->visualize) {
        double current_ms = (double)systime_ms();
        if (current_ms >= x->last_viz_check_ms + 333.33) {
            for (long t = 0; t < x->track_cache_count; t++) {
                t_weaver_track *tr = x->track_cache[t];
                if (!tr) continue;

                char l_msg[256] = "";
                char msg[256] = "";
                int has_l = 0, has_m = 0;

                // Capture data quickly under lock
                critical_enter(x->lock);
                if (tr->viz_trigger_dirty) {
                    snprintf(l_msg, sizeof(l_msg), "{\"track\": %ld, \"ms\": %.2f, \"palette\": \"%s\", \"offset\": %.0f, \"bar\": \"%s\", \"len\": %.0f, \"f2\": %.1f, \"busy\": %d}",
                             t + 1, tr->viz_ms, tr->viz_palette->s_name, tr->viz_offset, tr->viz_bar_symbol->s_name, tr->viz_track_length, (double)round(tr->viz_control), tr->viz_busy);
                    tr->viz_trigger_dirty = 0;
                    has_l = 1;
                }

                if (tr->viz_dirty) {
                    snprintf(msg, sizeof(msg), "{\"track\": %ld, \"ms\": %.2f, \"f1\": %.4f, \"f2\": %.4f, \"busy\": %d}",
                             t + 1, x->last_scan_val, tr->viz_f1, tr->viz_f2, tr->viz_busy);
                    tr->viz_dirty = 0;
                    has_m = 1;
                }
                critical_exit(x->lock);

                // Send data outside lock
                if (has_l) visualize(l_msg);
                if (has_m) visualize(msg);
            }
            x->last_viz_check_ms = current_ms;
        }
    }
}

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
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct _bar_cache {
    double value;
    t_symbol *sym;
} t_bar_cache;

typedef struct _fifo_entry {
    t_bar_cache bar;
    double range_end; // For SWEEP
    long type; // 0 for DATA, 1 for SWEEP
} t_fifo_entry;

typedef struct _weaver_track {
    t_crossfade_state xf;
    t_symbol *palette[2];
    double offset[2];
    double control;
    int busy;
    t_buffer_ref *src_refs[2];
} t_weaver_track;

typedef struct _weaver {
    t_pxobject t_obj;
    t_symbol *poly_prefix;
    long log;
    void *log_outlet;
    void *signal_outlet;
    t_hashtab *buffer_refs;
    t_buffer_ref *bar_buffer_ref;
    t_buffer_ref *track1_ref;

    t_symbol *audio_dict_name;
    double last_scan_val;
    t_fifo_entry hit_bars[4096];
    int fifo_head;
    int fifo_tail;
    t_qelem *audio_qelem;
    t_critical lock;
    t_hashtab *pending_silence;
    long bar_warn_sent;
    long dict_found;
    long poly_found;

    t_hashtab *track_states;
    double low_ms;
    double high_ms;
} t_weaver;

void *weaver_new(t_symbol *s, long argc, t_atom *argv);
void weaver_free(t_weaver *x);
void weaver_anything(t_weaver *x, t_symbol *s, long argc, t_atom *argv);
t_max_err weaver_notify(t_weaver *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void weaver_assist(t_weaver *x, void *b, long m, long a, char *s);
void weaver_log(t_weaver *x, const char *fmt, ...);
void weaver_process_data(t_weaver *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms);
void weaver_check_attachments(t_weaver *x, long report_error);
double weaver_get_bar_length(t_weaver *x);
void weaver_dsp64(t_weaver *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void weaver_perform64(t_weaver *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void weaver_audio_qtask(t_weaver *x);
void weaver_clear_pending_silence(t_weaver *x);
void weaver_schedule_silence(t_weaver *x, t_atom_long track, double ms);
t_weaver_track *weaver_get_track_state(t_weaver *x, t_atom_long track_id);
void weaver_clear_track_states(t_weaver *x);

static t_class *weaver_class;

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
            tr->offset[0] = 0.0;
            tr->palette[1] = gensym("-");
            tr->offset[1] = 0.0;
            tr->control = 0.0;
            tr->busy = 0;
            tr->src_refs[0] = buffer_ref_new((t_object *)x, _sym_nothing);
            tr->src_refs[1] = buffer_ref_new((t_object *)x, _sym_nothing);
            hashtab_store(x->track_states, s_track, (t_object *)tr);
        }
    }
    return tr;
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
            sysmem_freeptr(tr);
        }
    }
    if (keys) sysmem_freeptr(keys);
    hashtab_clear(x->track_states);
}

void weaver_clear_pending_silence(t_weaver *x) {
    if (!x->pending_silence) return;
    long num_ms = 0;
    t_symbol **ms_keys = NULL;
    hashtab_getkeys(x->pending_silence, &num_ms, &ms_keys);
    for (long i = 0; i < num_ms; i++) {
        t_hashtab *tracks = NULL;
        hashtab_lookup(x->pending_silence, ms_keys[i], (t_object**)&tracks);
        if (tracks) {
            hashtab_clear(tracks);
            object_free(tracks);
        }
    }
    if (ms_keys) sysmem_freeptr(ms_keys);
    hashtab_clear(x->pending_silence);
}

void weaver_schedule_silence(t_weaver *x, t_atom_long track, double ms) {
    if (!x->pending_silence) return;
    char ms_str[64];
    snprintf(ms_str, 64, "%ld", (long)ms);
    t_symbol *s_ms = gensym(ms_str);

    t_hashtab *tracks = NULL;
    if (hashtab_lookup(x->pending_silence, s_ms, (t_object**)&tracks) != MAX_ERR_NONE) {
        tracks = hashtab_new(0);
        hashtab_store(x->pending_silence, s_ms, (t_object*)tracks);
    }
    // Store track as a pointer key
    hashtab_store(tracks, (t_symbol*)(size_t)track, (t_object*)1);
}

// Helper function to send verbose log messages with prefix
void weaver_log(t_weaver *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "weaver~", fmt, args);
    va_end(args);
}

void weaver_check_attachments(t_weaver *x, long report_error) {
    // Dictionary check
    if (x->audio_dict_name != _sym_nothing) {
        t_dictionary *d = dictobj_findregistered_retain(x->audio_dict_name);
        if (d) {
            if (!x->dict_found) {
                weaver_log(x, "successfully found dictionary '%s'", x->audio_dict_name->s_name);
                x->dict_found = 1;
            }
            dictobj_release(d);
        } else {
            if (x->dict_found) x->dict_found = 0;
            if (report_error) {
                object_error((t_object *)x, "dictionary '%s' not found", x->audio_dict_name->s_name);
            }
        }
    }

    // Polybuffer check (via track1_ref)
    if (x->poly_prefix != _sym_nothing) {
        t_buffer_obj *b = buffer_ref_getobject(x->track1_ref);
        if (b) {
            if (!x->poly_found) {
                weaver_log(x, "successfully found polybuffer~ '%s' (via %s.1)", x->poly_prefix->s_name, x->poly_prefix->s_name);
                x->poly_found = 1;
            }
        } else {
            if (x->poly_found) x->poly_found = 0;
            if (report_error) {
                object_error((t_object *)x, "polybuffer~ '%s' not found (could not find %s.1)", x->poly_prefix->s_name, x->poly_prefix->s_name);
            }
        }
    }
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("weaver~", (method)weaver_new, (method)weaver_free, sizeof(t_weaver), 0L, A_GIMME, 0);

    class_addmethod(c, (method)weaver_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)weaver_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)weaver_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)weaver_assist, "assist", A_CANT, 0);

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
        x->poly_prefix = _sym_nothing;
        x->log = 0;
        x->audio_dict_name = _sym_nothing;
        x->last_scan_val = -1.0;
        x->fifo_head = 0;
        x->fifo_tail = 0;
        x->dict_found = 0;
        x->poly_found = 0;

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

        attr_args_process(x, argc, argv);

        // Create outlets from right to left
        if (x->log) {
            x->log_outlet = outlet_new((t_object *)x, NULL);
        } else {
            x->log_outlet = NULL;
        }
        x->signal_outlet = outlet_new((t_object *)x, "signal");

        if (x->poly_prefix != _sym_nothing) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            x->track1_ref = buffer_ref_new((t_object *)x, gensym(t1name));
        } else {
            x->track1_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        }

        // Initial search for mandatory objects
        weaver_check_attachments(x, 1);

        x->buffer_refs = hashtab_new(0);
        x->bar_buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        if (!buffer_ref_getobject(x->bar_buffer_ref)) {
            object_error((t_object *)x, "bar buffer~ not found");
        }

        critical_new(&x->lock);
        x->pending_silence = hashtab_new(0);
        x->bar_warn_sent = 0;

        x->track_states = hashtab_new(0);
        x->low_ms = 22.653;
        x->high_ms = 4999.0;

        dsp_setup((t_pxobject *)x, 1);
        x->audio_qelem = qelem_new(x, (method)weaver_audio_qtask);
    }
    return x;
}

void weaver_free(t_weaver *x) {
    dsp_free((t_pxobject *)x);
    critical_free(x->lock);
    if (x->audio_qelem) qelem_free(x->audio_qelem);

    if (x->buffer_refs) {
        long num_items = 0;
        t_symbol **keys = NULL;
        hashtab_getkeys(x->buffer_refs, &num_items, &keys);
        for (long i = 0; i < num_items; i++) {
            t_buffer_ref *ref = NULL;
            hashtab_lookup(x->buffer_refs, keys[i], (t_object **)&ref);
            if (ref) object_free(ref);
        }
        if (keys) sysmem_freeptr(keys);
        object_free(x->buffer_refs);
    }
    if (x->bar_buffer_ref) {
        object_free(x->bar_buffer_ref);
    }
    if (x->track1_ref) {
        object_free(x->track1_ref);
    }
    weaver_clear_pending_silence(x);
    if (x->pending_silence) object_free(x->pending_silence);
    weaver_clear_track_states(x);
    if (x->track_states) object_free(x->track_states);
}

t_max_err weaver_notify(t_weaver *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    if (x->buffer_refs) {
        long num_items = 0;
        t_symbol **keys = NULL;
        hashtab_getkeys(x->buffer_refs, &num_items, &keys);
        for (long i = 0; i < num_items; i++) {
            t_buffer_ref *ref = NULL;
            hashtab_lookup(x->buffer_refs, keys[i], (t_object **)&ref);
            if (ref) buffer_ref_notify(ref, s, msg, sender, data);
        }
        if (keys) sysmem_freeptr(keys);
    }
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
            case 0: sprintf(s, "Inlet 1 (signal): Time ramp (signal)"); break;
        }
    } else { // ASSIST_OUTLET
        if (x->log) {
            switch (a) {
                case 0: sprintf(s, "Outlet 1 (signal): Scan Head Position (ms)"); break;
                case 1: sprintf(s, "Outlet 2 (anything): Logging Outlet"); break;
            }
        } else {
            switch (a) {
                case 0: sprintf(s, "Outlet 1 (signal): Scan Head Position (ms)"); break;
            }
        }
    }
}

double weaver_get_bar_length(t_weaver *x) {
    double bar_length = 0;
    t_buffer_obj *b = buffer_ref_getobject(x->bar_buffer_ref);
    if (!b) {
        if (!x->bar_warn_sent) {
            object_warn((t_object *)x, "bar buffer~ not found, attempting to kick reference");
            x->bar_warn_sent = 1;
        }
        // Kick the buffer reference to force re-binding
        buffer_ref_set(x->bar_buffer_ref, _sym_nothing);
        buffer_ref_set(x->bar_buffer_ref, gensym("bar"));
        b = buffer_ref_getobject(x->bar_buffer_ref);
    }
    if (b) {
        x->bar_warn_sent = 0; // Reset flag when buffer is successfully found
        critical_enter(0);
        float *samples = buffer_locksamples(b);
        if (samples) {
            if (buffer_getframecount(b) > 0) {
                bar_length = (double)samples[0];
            }
            buffer_unlocksamples(b);
        }
        critical_exit(0);
    }
    return bar_length;
}


void weaver_process_data(t_weaver *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms) {
    weaver_check_attachments(x, 0);

    double bar_len = weaver_get_bar_length(x);
    if (bar_len <= 0) return;

    // 1. Destination Buffer Lookup
    char bufname[256];
    snprintf(bufname, 256, "%s.%lld", x->poly_prefix->s_name, (long long)track);
    t_symbol *s_bufname = gensym(bufname);

    t_buffer_ref *dest_ref = NULL;
    hashtab_lookup(x->buffer_refs, s_bufname, (t_object **)&dest_ref);
    if (!dest_ref) {
        dest_ref = buffer_ref_new((t_object *)x, s_bufname);
        hashtab_store(x->buffer_refs, s_bufname, (t_object *)dest_ref);
    }
    t_buffer_obj *dest_buf = buffer_ref_getobject(dest_ref);
    if (!dest_buf) {
        weaver_log(x, "Error: destination buffer %s not found", s_bufname->s_name);
        return;
    }

    double sr_dest = buffer_getsamplerate(dest_buf);
    if (sr_dest <= 0) sr_dest = sys_getsr();
    if (sr_dest <= 0) sr_dest = 44100.0;

    long n_frames_dest = buffer_getframecount(dest_buf);
    long n_chans_dest = buffer_getchannelcount(dest_buf);
    long start_frame = (long)round(bar_ms * sr_dest / 1000.0);
    long end_frame = (long)round((bar_ms + bar_len) * sr_dest / 1000.0);

    if (start_frame >= n_frames_dest) return;
    if (end_frame > n_frames_dest) end_frame = n_frames_dest;
    if (start_frame < 0) start_frame = 0;

    // 2. Track State and Crossfade Trigger
    t_weaver_track *tr = weaver_get_track_state(x, track);
    if (!tr) return;

    crossfade_update_params(&tr->xf, sr_dest, x->low_ms, x->high_ms);

    if (tr->busy) {
        weaver_log(x, "Track %lld: busy at %.2f ms, skipping transition to %s@%.2f", track, bar_ms, palette->s_name, offset_ms);
    } else {
        int active = (int)round(tr->control);
        if (palette == tr->palette[active] && offset_ms == tr->offset[active]) {
            // Same source, no action needed
        } else {
            int other = 1 - active;
            tr->palette[other] = palette;
            tr->offset[other] = offset_ms;
            tr->control = (double)other;
            // Set direction for the crossfade module to trigger on the first sample
            tr->xf.direction = tr->control - tr->xf.last_control;
            weaver_log(x, "Track %lld: starting crossfade at %.2f ms to %s@%.2f", track, bar_ms, palette->s_name, offset_ms);
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
        buffer_ref_set(tr->src_refs[i], tr->palette[i]);
        src_buf[i] = buffer_ref_getobject(tr->src_refs[i]);
        if (src_buf[i]) {
            sr_src[i] = buffer_getsamplerate(src_buf[i]);
            if (sr_src[i] <= 0) sr_src[i] = 44100.0;
            n_frames_src[i] = buffer_getframecount(src_buf[i]);
            n_chans_src[i] = buffer_getchannelcount(src_buf[i]);

            for (int retry = 0; retry < 10; retry++) {
                samples_src[i] = buffer_locksamples(src_buf[i]);
                if (samples_src[i]) break;
                systhread_sleep(1);
            }
        }
    }

    // Weave loop with crossfade and nearest-neighbor sample rate conversion
    for (long f = start_frame; f < end_frame; f++) {
        double current_ms = (double)f * 1000.0 / sr_dest;
        double max_abs[2] = {0.0, 0.0};
        long f_src[2] = {-1, -1};

        for (int i = 0; i < 2; i++) {
            if (samples_src[i]) {
                double src_ms = current_ms + tr->offset[i];
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


void weaver_anything(t_weaver *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) != 0) return;

    // Inlet 0: Rescript / Dictionary Reference
    if (s != x->audio_dict_name) {
        x->audio_dict_name = s;
        x->dict_found = 0;
    }
}

void weaver_dsp64(t_weaver *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    if (count[0]) {
        weaver_log(x, "DSP ON: tracking signal ramp");
        dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)weaver_perform64, 0, NULL);
    }
}

void weaver_perform64(t_weaver *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];
    double *out = outs[0];
    double last_scan = x->last_scan_val;

    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        for (int i = 0; i < sampleframes; i++) {
            double current_scan = in[i] + 9.0; // 9ms lookahead

            out[i] = current_scan; // Output scan head position (ms)

            // 1. DATA hits (integer bar crossings and wrap-around)
            if (last_scan != -1.0) {
                if (current_scan < last_scan) {
                    // Wrap-around: trigger bar 0
                    int next_tail = (x->fifo_tail + 1) % 4096;
                    if (next_tail != x->fifo_head) {
                        x->hit_bars[x->fifo_tail].bar.value = 0.0;
                        x->hit_bars[x->fifo_tail].bar.sym = NULL;
                        x->hit_bars[x->fifo_tail].type = 0; // DATA
                        x->fifo_tail = next_tail;
                    }

                    // Also handle any integers between 0 and current_scan that we might have jumped to
                    long floor_curr = (long)floor(current_scan);
                    for (long v = 1; v <= floor_curr; v++) {
                        int nt = (x->fifo_tail + 1) % 4096;
                        if (nt != x->fifo_head) {
                            x->hit_bars[x->fifo_tail].bar.value = (double)v;
                            x->hit_bars[x->fifo_tail].bar.sym = NULL;
                            x->hit_bars[x->fifo_tail].type = 0; // DATA
                            x->fifo_tail = nt;
                        }
                    }
                } else if (current_scan > last_scan) {
                    long floor_last = (long)floor(last_scan);
                    long floor_curr = (long)floor(current_scan);

                    if (floor_curr > floor_last) {
                        for (long v = floor_last + 1; v <= floor_curr; v++) {
                            int next_tail = (x->fifo_tail + 1) % 4096;
                            if (next_tail != x->fifo_head) {
                                x->hit_bars[x->fifo_tail].bar.value = (double)v;
                                x->hit_bars[x->fifo_tail].bar.sym = NULL;
                                x->hit_bars[x->fifo_tail].type = 0; // DATA
                                x->fifo_tail = next_tail;
                            }
                        }
                    }
                }
            }

            last_scan = current_scan;
        }

        qelem_set(x->audio_qelem);

        x->last_scan_val = last_scan;
        critical_exit(x->lock);
    }

}

void weaver_audio_qtask(t_weaver *x) {
    weaver_check_attachments(x, 0);
    while (x->fifo_head != x->fifo_tail) {
        t_fifo_entry hit_entry = x->hit_bars[x->fifo_head];
        x->fifo_head = (x->fifo_head + 1) % 4096;

        t_bar_cache hit = hit_entry.bar;

        // DATA Hit logic
        t_symbol *bar_key = hit.sym;
        if (!bar_key) {
            char bstr[64];
            snprintf(bstr, 64, "%ld", (long)hit.value);
            bar_key = gensym(bstr);
        }

        // Check for scheduled silence caps at this millisecond
        t_hashtab *pending_tracks = NULL;
        if (hashtab_lookup(x->pending_silence, bar_key, (t_object**)&pending_tracks) == MAX_ERR_NONE && pending_tracks) {
            long num_pt = 0;
            t_symbol **pt_keys = NULL;
            hashtab_getkeys(pending_tracks, &num_pt, &pt_keys);

            t_dictionary *s_dict = dictobj_findregistered_retain(x->audio_dict_name);

            for (long i = 0; i < num_pt; i++) {
                t_atom_long track_num = (t_atom_long)(size_t)pt_keys[i];
                int still_missing = 1;

                if (s_dict) {
                    char tstr[64];
                    snprintf(tstr, 64, "%ld", (long)track_num);
                    t_symbol *track_sym = gensym(tstr);
                    t_dictionary *track_dict = NULL;

                    if (dictionary_getdictionary(s_dict, track_sym, (t_object **)&track_dict) == MAX_ERR_NONE && track_dict) {
                        if (dictionary_hasentry(track_dict, bar_key)) {
                            still_missing = 0;
                        }
                    }
                }

                if (still_missing) {
                    weaver_process_data(x, gensym("-"), track_num, hit.value, 0.0);
                }
            }
            if (s_dict) dictobj_release(s_dict);

            if (pt_keys) sysmem_freeptr(pt_keys);
            hashtab_clear(pending_tracks);
            object_free(pending_tracks);
            hashtab_delete(x->pending_silence, bar_key);
        }

        t_dictionary *dict = dictobj_findregistered_retain(x->audio_dict_name);
        if (!dict) {
            weaver_log(x, "ERROR: dictionary '%s' not found", x->audio_dict_name->s_name);
            continue;
        }

        double bar_len = weaver_get_bar_length(x);

        long num_tracks = 0;
        t_symbol **track_keys = NULL;
        dictionary_getkeys(dict, &num_tracks, &track_keys);

        for (long i = 0; i < num_tracks; i++) {
            t_symbol *track_sym = track_keys[i];
            t_atom_long track_val = atoll(track_sym->s_name);
            t_dictionary *track_dict = NULL;

            if (dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) {
                continue;
            }

            t_dictionary *bar_dict = NULL;
            if (dictionary_getdictionary(track_dict, bar_key, (t_object **)&bar_dict) == MAX_ERR_NONE && bar_dict) {
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

                weaver_process_data(x, palette, (t_atom_long)track_val, hit.value, offset);

                // Silence Cap logic: schedule silence if the next bar is missing from the dictionary
                double next_bar_ms = hit.value + bar_len;
                char next_bar_str[64];
                snprintf(next_bar_str, 64, "%ld", (long)next_bar_ms);
                t_symbol *next_bar_sym = gensym(next_bar_str);

                if (!dictionary_hasentry(track_dict, next_bar_sym)) {
                    weaver_schedule_silence(x, (t_atom_long)track_val, next_bar_ms);
                }
            }
        }
        if (track_keys) sysmem_freeptr(track_keys);
        dictobj_release(dict);
    }
}

#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include "../shared/crossfade.h"
#include <string.h>
#include <math.h>

typedef struct _voice {
    t_buffer_ref *play_ref;
    t_symbol *play_sym;
    double playhead;
    int playing;
    long long bar_start;
    long long bar_end;
} t_voice;

typedef struct _skipsilence {
    t_pxobject b_obj;
    t_voice voices[2];
    int active_idx;
    t_buffer_ref *scan_ref;
    t_buffer_ref *bar_ref;

    long log;
    void *log_outlet;
    void *playhead_outlet;

    // Crossfade state
    t_crossfade_state xfade_l, xfade_r;
    double xfade_target;
    int xfade_busy;
    double xfade_low, xfade_high;

    // Playback state
    int play_mode;
    t_symbol *pending_play_sym;
    t_symbol *scan_sym;

    // Scanner state
    long long next_bar_start;
    long long next_bar_end;
    int next_bar_ready;
    int scanner_trigger;
    long long scan_from;

    int scanner_should_exit;
    t_systhread scanner_thread;
    t_critical lock;

    void *proxy;
    long proxy_id;

} t_skipsilence;

void *skipsilence_new(t_symbol *s, long argc, t_atom *argv);
void skipsilence_free(t_skipsilence *x);
void skipsilence_play(t_skipsilence *x, t_symbol *s);
void skipsilence_int(t_skipsilence *x, long n);
void skipsilence_dsp64(t_skipsilence *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void skipsilence_perform64(t_skipsilence *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
t_max_err skipsilence_notify(t_skipsilence *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void skipsilence_assist(t_skipsilence *x, void *b, long m, long a, char *s);
t_max_err skipsilence_attr_set_xfade(t_skipsilence *x, void *attr, long argc, t_atom *argv);

void skipsilence_log(t_skipsilence *x, const char *fmt, ...);
void *skipsilence_scanner_thread(t_skipsilence *x);

static t_class *skipsilence_class;

void ext_main(void *r) {
    common_symbols_init();

    t_class *c = class_new("skipsilence~", (method)skipsilence_new, (method)skipsilence_free, sizeof(t_skipsilence), 0L, A_GIMME, 0);

    class_addmethod(c, (method)skipsilence_play, "play", A_SYM, 0);
    class_addmethod(c, (method)skipsilence_int, "int", A_LONG, 0);
    class_addmethod(c, (method)skipsilence_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)skipsilence_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)skipsilence_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_skipsilence, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_DOUBLE(c, "xfade_low", 0, t_skipsilence, xfade_low);
    CLASS_ATTR_LABEL(c, "xfade_low", 0, "Crossfade Low Time (ms)");
    CLASS_ATTR_ACCESSORS(c, "xfade_low", NULL, (method)skipsilence_attr_set_xfade);
    CLASS_ATTR_DEFAULT(c, "xfade_low", 0, "10.0");

    CLASS_ATTR_DOUBLE(c, "xfade_high", 0, t_skipsilence, xfade_high);
    CLASS_ATTR_LABEL(c, "xfade_high", 0, "Crossfade High Time (ms)");
    CLASS_ATTR_ACCESSORS(c, "xfade_high", NULL, (method)skipsilence_attr_set_xfade);
    CLASS_ATTR_DEFAULT(c, "xfade_high", 0, "200.0");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    skipsilence_class = c;
}

t_max_err skipsilence_attr_set_xfade(t_skipsilence *x, void *attr, long argc, t_atom *argv) {
    if (argc && argv) {
        double val = atom_getfloat(argv);
        t_symbol *name = (t_symbol *)object_method(attr, gensym("getname"));

        if (name == gensym("xfade_low")) x->xfade_low = val;
        else if (name == gensym("xfade_high")) x->xfade_high = val;

        double sr = sys_getsr();
        if (sr <= 0) sr = 44100.0;
        crossfade_update_params(&x->xfade_l, sr, x->xfade_low, x->xfade_high);
        crossfade_update_params(&x->xfade_r, sr, x->xfade_low, x->xfade_high);
    }
    return MAX_ERR_NONE;
}

void skipsilence_log(t_skipsilence *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "skipsilence~", fmt, args);
    va_end(args);
}

void *skipsilence_new(t_symbol *s, long argc, t_atom *argv) {
    t_skipsilence *x = (t_skipsilence *)object_alloc(skipsilence_class);

    if (x) {
        dsp_setup((t_pxobject *)x, 0);
        x->proxy = proxy_new((t_object *)x, 1, &x->proxy_id);

        // Create outlets from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);           // Index 3
        x->playhead_outlet = outlet_new((t_object *)x, "signal");  // Index 2
        outlet_new((t_object *)x, "signal");                       // Index 1 (Right)
        outlet_new((t_object *)x, "signal");                       // Index 0 (Left)

        x->voices[0].play_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->voices[1].play_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->scan_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->bar_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        x->log = 0;
        x->active_idx = 0;
        x->xfade_target = 0.0;
        x->xfade_busy = 0;
        x->xfade_low = 10.0;
        x->xfade_high = 200.0;

        double sr = sys_getsr();
        if (sr <= 0) sr = 44100.0;
        crossfade_init(&x->xfade_l, sr, x->xfade_low, x->xfade_high);
        crossfade_init(&x->xfade_r, sr, x->xfade_low, x->xfade_high);

        for (int i = 0; i < 2; i++) {
            x->voices[i].playhead = 0;
            x->voices[i].playing = 0;
            x->voices[i].bar_start = -1;
            x->voices[i].bar_end = -1;
            x->voices[i].play_sym = _sym_nothing;
        }

        x->play_mode = 0;
        x->pending_play_sym = NULL;
        x->scan_sym = _sym_nothing;
        x->next_bar_start = -1;
        x->next_bar_end = -1;
        x->next_bar_ready = 0;
        x->scanner_trigger = 0;
        x->scan_from = 0;

        critical_new(&x->lock);
        x->scanner_should_exit = 0;

        attr_args_process(x, argc, argv);

        systhread_create((method)skipsilence_scanner_thread, x, 0, 0, 0, &x->scanner_thread);
    }
    return x;
}

void skipsilence_free(t_skipsilence *x) {
    dsp_free((t_pxobject *)x);

    x->scanner_should_exit = 1;
    if (x->scanner_thread) {
        systhread_join(x->scanner_thread, NULL);
    }

    critical_free(x->lock);

    if (x->voices[0].play_ref) object_free(x->voices[0].play_ref);
    if (x->voices[1].play_ref) object_free(x->voices[1].play_ref);
    if (x->scan_ref) object_free(x->scan_ref);
    if (x->bar_ref) object_free(x->bar_ref);
    if (x->proxy) object_free(x->proxy);
}

t_max_err skipsilence_notify(t_skipsilence *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    if (x->voices[0].play_ref) buffer_ref_notify(x->voices[0].play_ref, s, msg, sender, data);
    if (x->voices[1].play_ref) buffer_ref_notify(x->voices[1].play_ref, s, msg, sender, data);
    if (x->scan_ref) buffer_ref_notify(x->scan_ref, s, msg, sender, data);
    if (x->bar_ref) buffer_ref_notify(x->bar_ref, s, msg, sender, data);
    return MAX_ERR_NONE;
}

void skipsilence_assist(t_skipsilence *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1 (messages): play [buffer_name]"); break;
            case 1: sprintf(s, "Inlet 2 (int): Play Mode (0: once, 1: repeat, 2: stop)"); break;
        }
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (signal): Left Channel (crossfaded)"); break;
            case 1: sprintf(s, "Outlet 2 (signal): Right Channel (crossfaded)"); break;
            case 2: sprintf(s, "Outlet 3 (signal): Playhead (milliseconds)"); break;
            case 3: sprintf(s, "Outlet 4 (anything): Logging Outlet"); break;
        }
    }
}

void skipsilence_int(t_skipsilence *x, long n) {
    long inlet = proxy_getinlet((t_object *)x);
    if (inlet == 1) {
        critical_enter(x->lock);
        x->play_mode = (int)n;
        if (x->play_mode == 2) {
            if (x->voices[x->active_idx].playing) {
                skipsilence_log(x, "INT: stop mode (2) received, deferring stop until next bar");
            } else {
                x->scanner_trigger = 0;
                x->next_bar_ready = 0;
                skipsilence_log(x, "INT: stop mode (2) received, stopping immediately");
            }
        } else {
            skipsilence_log(x, "INT: play mode set to %d", x->play_mode);
        }
        critical_exit(x->lock);
    }
}

void skipsilence_play(t_skipsilence *x, t_symbol *s) {
    critical_enter(x->lock);
    if (x->voices[x->active_idx].playing) {
        x->pending_play_sym = s;
        buffer_ref_set(x->scan_ref, s);
        x->scan_sym = s;
        skipsilence_log(x, "PLAY: deferring playback of buffer '%s' until next bar", s->s_name);
    } else {
        buffer_ref_set(x->voices[0].play_ref, s);
        x->voices[0].play_sym = s;
        buffer_ref_set(x->scan_ref, s);
        x->scan_sym = s;

        t_buffer_obj *b = buffer_ref_getobject(x->voices[0].play_ref);
        if (!b) {
            object_error((t_object *)x, "buffer ~ %s not found", s->s_name);
            x->voices[0].playing = 0;
            critical_exit(x->lock);
            return;
        }

        x->active_idx = 1; // Start at index 1 (silent)
        x->xfade_target = 1.0; // Currently at voice 1
        x->xfade_busy = 0;

        for (int i = 0; i < 2; i++) {
            x->voices[i].playing = 0;
            x->voices[i].playhead = 0;
            x->voices[i].bar_start = -1;
            x->voices[i].bar_end = -1;
            if (i > 0) x->voices[i].play_sym = _sym_nothing;
        }

        x->next_bar_start = -1;
        x->next_bar_end = -1;
        x->next_bar_ready = 0;
        x->scan_from = 0;
        x->scanner_trigger = 1;
        skipsilence_log(x, "PLAY: starting playback of buffer '%s' immediately (fade-in)", s->s_name);
    }
    critical_exit(x->lock);
}

void skipsilence_dsp64(t_skipsilence *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)skipsilence_perform64, 0, NULL);
}

void skipsilence_perform64(t_skipsilence *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    t_buffer_obj *b[2];
    float *samples[2] = {NULL, NULL};
    long long n_frames[2] = {0, 0};
    long n_chans[2] = {0, 0};
    double sr[2] = {0, 0};

    for (int v = 0; v < 2; v++) {
        b[v] = buffer_ref_getobject(x->voices[v].play_ref);
        sr[v] = sys_getsr();
        if (b[v]) {
            samples[v] = buffer_locksamples(b[v]);
            if (samples[v]) {
                n_frames[v] = buffer_getframecount(b[v]);
                n_chans[v] = buffer_getchannelcount(b[v]);
                sr[v] = buffer_getsamplerate(b[v]);
                if (sr[v] <= 0) sr[v] = sys_getsr();
            }
        }
    }

    critical_enter(x->lock);

    for (int i = 0; i < sampleframes; i++) {
        // 1. Transition Logic
        if (x->voices[x->active_idx].playing && x->voices[x->active_idx].playhead >= (double)x->voices[x->active_idx].bar_end) {
            int v = x->active_idx;
            int next_v = 1 - x->active_idx;
            if (x->pending_play_sym) {
                skipsilence_log(x, "PERFORM: switching to pending buffer '%s' (fade-out to wait for scanner)", x->pending_play_sym->s_name);
                buffer_ref_set(x->voices[0].play_ref, x->pending_play_sym);
                x->voices[0].play_sym = x->pending_play_sym;
                x->voices[0].playing = 0;
                x->voices[1].playing = 0;
                x->next_bar_ready = 0;
                x->scan_from = 0;
                x->scanner_trigger = 1;
                x->pending_play_sym = NULL;
                x->active_idx = 1;
                x->xfade_target = 1.0;
                x->xfade_busy = 1;
            } else if (x->play_mode == 2) {
                skipsilence_log(x, "PERFORM: enacting deferred stop (fade-out)");
                x->voices[v].playing = 0;
                x->next_bar_ready = 0;
                x->scanner_trigger = 0;
                x->active_idx = next_v;
                x->xfade_target = (double)next_v;
                x->xfade_busy = 1;
            } else if (x->next_bar_ready) {
                skipsilence_log(x, "PERFORM: switching to next bar");
                buffer_ref_set(x->voices[next_v].play_ref, x->voices[v].play_sym);
                x->voices[next_v].play_sym = x->voices[v].play_sym;
                x->voices[next_v].bar_start = x->next_bar_start;
                x->voices[next_v].bar_end = x->next_bar_end;
                x->voices[next_v].playhead = (double)x->voices[next_v].bar_start;
                x->voices[next_v].playing = 1;
                x->voices[v].playing = 0;
                x->next_bar_ready = 0;
                x->scan_from = x->voices[next_v].bar_end;
                x->scanner_trigger = 1;
                x->active_idx = next_v;
                x->xfade_target = (double)next_v;
                x->xfade_busy = 1;
            } else if (x->play_mode == 1) {
                x->voices[v].playhead = (double)x->voices[v].bar_end;
            } else {
                skipsilence_log(x, "PERFORM: end reached, next bar not ready. stopping.");
                x->voices[v].playing = 0;
                x->active_idx = next_v;
                x->xfade_target = (double)next_v;
                x->xfade_busy = 1;
            }
        }

        // 2. Fetch Samples
        double v_out_l[2] = {0, 0};
        double v_out_r[2] = {0, 0};
        for (int v = 0; v < 2; v++) {
            if ((x->voices[v].playing || (v != x->active_idx && x->xfade_busy)) && samples[v]) {
                long long f = (long long)x->voices[v].playhead;
                if (f >= 0 && f < n_frames[v] && (x->voices[v].playing || f < x->voices[v].bar_end)) {
                    if (n_chans[v] >= 2) {
                        v_out_l[v] = (double)samples[v][f * n_chans[v]];
                        v_out_r[v] = (double)samples[v][f * n_chans[v] + 1];
                    } else if (n_chans[v] == 1) {
                        v_out_l[v] = v_out_r[v] = (double)samples[v][f];
                    }
                }
                x->voices[v].playhead += 1.0;
            }
        }

        // 3. Crossfade and Output
        double mix_l[2], mix_r[2], sum_l, sum_r;
        int busy_l, busy_r;

        crossfade_process(&x->xfade_l, x->xfade_target, v_out_l[0], v_out_l[1], &mix_l[0], &mix_l[1], &sum_l, &busy_l);
        crossfade_process(&x->xfade_r, x->xfade_target, v_out_r[0], v_out_r[1], &mix_r[0], &mix_r[1], &sum_r, &busy_r);

        x->xfade_busy = busy_l || busy_r;

        outs[0][i] = sum_l;
        outs[1][i] = sum_r;
        outs[2][i] = x->voices[x->active_idx].playing ? (x->voices[x->active_idx].playhead * 1000.0 / sr[x->active_idx]) : 0.0;
    }
    critical_exit(x->lock);

    for (int v = 0; v < 2; v++) {
        if (b[v] && samples[v]) buffer_unlocksamples(b[v]);
    }
}

void *skipsilence_scanner_thread(t_skipsilence *x) {
    while (!x->scanner_should_exit) {
        int trigger = 0;
        critical_enter(x->lock);
        trigger = x->scanner_trigger && !x->next_bar_ready;
        critical_exit(x->lock);

        if (trigger) {
            double bar_ms = 0;
            t_buffer_obj *bar_b = buffer_ref_getobject(x->bar_ref);
            if (bar_b) {
                float *bar_s = buffer_locksamples(bar_b);
                if (bar_s) {
                    if (buffer_getframecount(bar_b) > 0) {
                        bar_ms = (double)bar_s[0];
                    }
                    buffer_unlocksamples(bar_b);
                }
            }

            if (bar_ms <= 0) {
                systhread_sleep(10);
                continue;
            }

            t_buffer_obj *play_b = buffer_ref_getobject(x->scan_ref);
            if (!play_b) {
                systhread_sleep(10);
                continue;
            }

            float *play_s = buffer_locksamples(play_b);
            if (!play_s) {
                systhread_sleep(10);
                continue;
            }

            long long total_frames = buffer_getframecount(play_b);
            long chans = buffer_getchannelcount(play_b);
            double sr = buffer_getsamplerate(play_b);
            if (sr <= 0) sr = sys_getsr();

            long long bar_frames = (long long)ceil(bar_ms * sr / 1000.0);
            if (bar_frames <= 0) bar_frames = 1; // Safety

            long long b_start;
            critical_enter(x->lock);
            b_start = x->scan_from;
            critical_exit(x->lock);

            if (b_start >= total_frames) {
                buffer_unlocksamples(play_b);
                critical_enter(x->lock);
                if (x->play_mode == 1) {
                    x->scan_from = 0;
                    x->scanner_trigger = 1;
                    skipsilence_log(x, "SCAN: repeat mode (1) active, restarting scan from beginning");
                } else {
                    x->scanner_trigger = 0;
                    skipsilence_log(x, "SCAN: reached end of buffer at %.2f ms", (double)total_frames * 1000.0 / sr);
                }
                critical_exit(x->lock);
                continue;
            }

            long long b_end = b_start + bar_frames;
            // Removed clipping of b_end to total_frames

            skipsilence_log(x, "SCAN: checking segment %.2f to %.2f ms (bar_ms: %.2f)", (double)b_start * 1000.0 / sr, (double)b_end * 1000.0 / sr, bar_ms);

            int has_audio = 0;
            for (long long f = b_start; f < b_end; f++) {
                if (f < total_frames) {
                    for (long c = 0; c < chans; c++) {
                        if (fabs(play_s[f * chans + c]) > 0.00009) {
                            has_audio = 1;
                            break;
                        }
                    }
                }
                if (has_audio) break;
            }

            buffer_unlocksamples(play_b);

            critical_enter(x->lock);
            if (has_audio) {
                x->next_bar_start = b_start;
                x->next_bar_end = b_end;
                x->next_bar_ready = 1;
                x->scanner_trigger = 0;

                if (!x->voices[0].playing && !x->voices[1].playing) {
                    // Both voices silent, start with voice 0 and fade in
                    buffer_ref_set(x->voices[0].play_ref, x->scan_sym);
                    x->voices[0].play_sym = x->scan_sym;
                    x->voices[0].bar_start = x->next_bar_start;
                    x->voices[0].bar_end = x->next_bar_end;
                    x->voices[0].playhead = (double)x->voices[0].bar_start;
                    x->voices[0].playing = 1;

                    x->active_idx = 0;
                    x->xfade_target = 0.0;

                    x->next_bar_ready = 0;
                    x->scan_from = x->voices[0].bar_end;
                    x->scanner_trigger = 1;
                    skipsilence_log(x, "SCAN: found audio, starting playback at %.2f to %.2f ms (fade-in)", (double)x->voices[0].bar_start * 1000.0 / sr, (double)x->voices[0].bar_end * 1000.0 / sr);
                } else {
                    skipsilence_log(x, "SCAN: found next audio bar at %.2f to %.2f ms", (double)x->next_bar_start * 1000.0 / sr, (double)x->next_bar_end * 1000.0 / sr);
                }
            } else {
                skipsilence_log(x, "SCAN: segment %.2f to %.2f ms was silent. skipping...", (double)b_start * 1000.0 / sr, (double)b_end * 1000.0 / sr);
                x->scan_from = b_end;
                if (x->scan_from >= total_frames) {
                    x->scanner_trigger = 0;
                    // If we were playing and reached the end of scanning, x->playing will be set to 0 in perform routine when current bar ends
                }
                // Continue scanning in next iteration (x->scanner_trigger remains 1)
            }
            critical_exit(x->lock);
        } else {
            systhread_sleep(2);
        }
    }
    return NULL;
}

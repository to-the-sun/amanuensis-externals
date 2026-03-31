#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>

typedef struct _skipsilence {
    t_pxobject b_obj;
    t_buffer_ref *play_ref;
    t_buffer_ref *bar_ref;

    long log;
    void *log_outlet;
    void *playhead_outlet;

    // Playback state
    double playhead; // In frames
    int playing;
    int play_mode;
    t_symbol *pending_play_sym;
    long long current_bar_start;
    long long current_bar_end;

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

    int zero_bar_mode;
    double last_bar_ms;

} t_skipsilence;

void *skipsilence_new(t_symbol *s, long argc, t_atom *argv);
void skipsilence_free(t_skipsilence *x);
void skipsilence_play(t_skipsilence *x, t_symbol *s);
void skipsilence_int(t_skipsilence *x, long n);
void skipsilence_dsp64(t_skipsilence *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void skipsilence_perform64(t_skipsilence *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
t_max_err skipsilence_notify(t_skipsilence *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void skipsilence_assist(t_skipsilence *x, void *b, long m, long a, char *s);

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

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    skipsilence_class = c;
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

        x->play_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->bar_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        x->log = 0;
        x->playhead = 0;
        x->playing = 0;
        x->play_mode = 0;
        x->pending_play_sym = NULL;
        x->current_bar_start = -1;
        x->current_bar_end = -1;
        x->next_bar_start = -1;
        x->next_bar_end = -1;
        x->next_bar_ready = 0;
        x->scanner_trigger = 0;
        x->scan_from = 0;
        x->zero_bar_mode = 0;
        x->last_bar_ms = -1;

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

    if (x->play_ref) object_free(x->play_ref);
    if (x->bar_ref) object_free(x->bar_ref);
    if (x->proxy) object_free(x->proxy);
}

t_max_err skipsilence_notify(t_skipsilence *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    if (x->play_ref) buffer_ref_notify(x->play_ref, s, msg, sender, data);
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
            case 0: sprintf(s, "Outlet 1 (signal): Left Channel"); break;
            case 1: sprintf(s, "Outlet 2 (signal): Right Channel"); break;
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
            if (x->playing) {
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
    if (x->playing) {
        x->pending_play_sym = s;
        skipsilence_log(x, "PLAY: deferring playback of buffer '%s' until next bar", s->s_name);
    } else {
        buffer_ref_set(x->play_ref, s);

        t_buffer_obj *b = buffer_ref_getobject(x->play_ref);
        if (!b) {
            object_error((t_object *)x, "buffer ~ %s not found", s->s_name);
            x->playing = 0;
            critical_exit(x->lock);
            return;
        }

        x->playing = 0;
        x->playhead = 0;
        x->current_bar_start = -1;
        x->current_bar_end = -1;
        x->next_bar_start = -1;
        x->next_bar_end = -1;
        x->next_bar_ready = 0;
        x->scan_from = 0;
        x->scanner_trigger = 1;
        x->zero_bar_mode = 0;
        skipsilence_log(x, "PLAY: starting playback of buffer '%s' immediately", s->s_name);
    }
    critical_exit(x->lock);
}

void skipsilence_dsp64(t_skipsilence *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)skipsilence_perform64, 0, NULL);
}

void skipsilence_perform64(t_skipsilence *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    t_buffer_obj *b = buffer_ref_getobject(x->play_ref);
    float *samples = NULL;
    long long n_frames = 0;
    long n_chans = 0;
    double sr = sys_getsr();

    if (b) {
        samples = buffer_locksamples(b);
        if (samples) {
            n_frames = buffer_getframecount(b);
            n_chans = buffer_getchannelcount(b);
            sr = buffer_getsamplerate(b);
            if (sr <= 0) sr = sys_getsr();
        }
    }

    critical_enter(x->lock);
    for (int i = 0; i < sampleframes; i++) {
        double out_l = 0, out_r = 0;
        if (x->playing && samples) {
            long long f = (long long)x->playhead;
            if (f >= 0 && f < n_frames) {
                if (n_chans >= 2) {
                    out_l = (double)samples[f * n_chans];
                    out_r = (double)samples[f * n_chans + 1];
                } else if (n_chans == 1) {
                    out_l = out_r = (double)samples[f];
                }
            }

            if (x->playing) {
                x->playhead += 1.0;
                if (x->playhead >= (double)x->current_bar_end) {
                    if (x->pending_play_sym) {
                        skipsilence_log(x, "PERFORM: switching to pending buffer '%s'", x->pending_play_sym->s_name);
                        buffer_ref_set(x->play_ref, x->pending_play_sym);
                        x->playhead = 0;
                        x->current_bar_start = -1;
                        x->current_bar_end = -1;
                        x->next_bar_ready = 0;
                        x->scan_from = 0;
                        x->scanner_trigger = 1;
                        x->playing = 0;
                        x->pending_play_sym = NULL;
                        x->zero_bar_mode = 0;
                    } else if (x->play_mode == 2) {
                        skipsilence_log(x, "PERFORM: enacting deferred stop");
                        x->playing = 0;
                        x->next_bar_ready = 0;
                        x->scanner_trigger = 0;
                    } else if (x->next_bar_ready) {
                        double ms_s = (double)x->next_bar_start * 1000.0 / sr;
                        double ms_e = (double)x->next_bar_end * 1000.0 / sr;
                        skipsilence_log(x, "PERFORM: switching to next bar %.2f to %.2f ms", ms_s, ms_e);
                        x->current_bar_start = x->next_bar_start;
                        x->current_bar_end = x->next_bar_end;
                        x->playhead = (double)x->current_bar_start;
                        x->next_bar_ready = 0;
                        x->scan_from = x->current_bar_end;
                        x->scanner_trigger = 1;
                    } else if (x->play_mode == 1) {
                        if (x->zero_bar_mode) {
                            x->playhead = 0;
                            x->current_bar_start = -1;
                            x->current_bar_end = -1;
                            x->scan_from = 0;
                            x->scanner_trigger = 1;
                            x->playing = 0;
                            skipsilence_log(x, "PERFORM: zero-bar mode repeat, restarting scan from beginning");
                        } else {
                            // Repeat mode: wait for scanner at bar boundary
                            x->playhead = (double)x->current_bar_end;
                        }
                    } else {
                        skipsilence_log(x, "PERFORM: end of current bar reached, next bar not ready. stopping.");
                        x->playing = 0;
                    }
                }
            }
        }
        outs[0][i] = out_l;
        outs[1][i] = out_r;
        outs[2][i] = x->playing ? (x->playhead * 1000.0 / sr) : 0.0;
    }
    critical_exit(x->lock);

    if (b && samples) buffer_unlocksamples(b);
}

void *skipsilence_scanner_thread(t_skipsilence *x) {
    while (!x->scanner_should_exit) {
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

        t_buffer_obj *play_b = buffer_ref_getobject(x->play_ref);

        critical_enter(x->lock);
        int bar_length_changed = (bar_ms != x->last_bar_ms);
        x->last_bar_ms = bar_ms;
        critical_exit(x->lock);

        if (bar_ms <= 0) {
            critical_enter(x->lock);
            if (!x->zero_bar_mode) {
                skipsilence_log(x, "SCAN: entering zero-bar mode (bar_length <= 0)");
                x->zero_bar_mode = 1;
                if (x->playing) {
                    t_buffer_obj *pb = buffer_ref_getobject(x->play_ref);
                    if (pb) {
                        x->current_bar_end = buffer_getframecount(pb);
                        x->scanner_trigger = 0;
                        skipsilence_log(x, "SCAN: zero-bar mode: extending current bar to buffer end (%lld frames)", x->current_bar_end);
                    }
                }
            }

            if (x->scanner_trigger && !x->next_bar_ready && !x->playing && play_b) {
                float *play_s = buffer_locksamples(play_b);
                if (play_s) {
                    long long total_frames = buffer_getframecount(play_b);
                    long chans = buffer_getchannelcount(play_b);
                    double sr = buffer_getsamplerate(play_b);
                    if (sr <= 0) sr = sys_getsr();

                    long long found_f = -1;
                    for (long long f = x->scan_from; f < total_frames; f++) {
                        for (long c = 0; c < chans; c++) {
                            if (fabs(play_s[f * chans + c]) > 0.00009) {
                                found_f = f;
                                break;
                            }
                        }
                        if (found_f != -1) break;
                    }
                    buffer_unlocksamples(play_b);

                    if (found_f != -1) {
                        x->current_bar_start = found_f;
                        x->current_bar_end = total_frames;
                        x->playhead = (double)found_f;
                        x->playing = 1;
                        x->scanner_trigger = 0;
                        skipsilence_log(x, "SCAN (Zero Bar): found leading audio at %.2f ms, playing to end", (double)found_f * 1000.0 / sr);
                    } else {
                        if (x->play_mode == 1) {
                            x->scan_from = 0;
                            skipsilence_log(x, "SCAN (Zero Bar): no audio found, repeat mode active, restarting from 0");
                        } else {
                            x->scanner_trigger = 0;
                            skipsilence_log(x, "SCAN (Zero Bar): no audio found, stopping scan");
                        }
                    }
                }
            }
            critical_exit(x->lock);
            systhread_sleep(1);
            continue;
        }

        // bar_ms > 0
        int trigger = 0;
        critical_enter(x->lock);
        if (x->zero_bar_mode || bar_length_changed) {
            if (x->zero_bar_mode) {
                skipsilence_log(x, "SCAN: exiting zero-bar mode (bar_length > 0)");
                x->zero_bar_mode = 0;
            } else {
                skipsilence_log(x, "SCAN: bar length changed, re-aligning");
            }
            if (x->playing && play_b) {
                double sr = buffer_getsamplerate(play_b);
                if (sr <= 0) sr = sys_getsr();
                long long bar_frames = (long long)ceil(bar_ms * sr / 1000.0);
                if (bar_frames <= 0) bar_frames = 1;

                long long bars_passed = (long long)x->playhead / bar_frames;
                x->current_bar_end = (bars_passed + 1) * bar_frames;
                x->scan_from = x->current_bar_end;
                x->scanner_trigger = 1;
                skipsilence_log(x, "SCAN: re-aligned to bar boundary: next bar starts at %.2f ms", (double)x->current_bar_end * 1000.0 / sr);
            }
        }
        trigger = x->scanner_trigger && !x->next_bar_ready;
        critical_exit(x->lock);

        if (trigger) {
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

                if (!x->playing) {
                    x->current_bar_start = x->next_bar_start;
                    x->current_bar_end = x->next_bar_end;
                    x->playhead = (double)x->current_bar_start;
                    x->playing = 1;
                    x->next_bar_ready = 0;
                    x->scan_from = x->current_bar_end;
                    x->scanner_trigger = 1;
                    skipsilence_log(x, "SCAN: found audio, starting playback at %.2f to %.2f ms", (double)x->current_bar_start * 1000.0 / sr, (double)x->current_bar_end * 1000.0 / sr);
                } else {
                    skipsilence_log(x, "SCAN: found next audio bar at %.2f to %.2f ms", (double)x->next_bar_start * 1000.0 / sr, (double)x->next_bar_end * 1000.0 / sr);
                }
            } else {
                skipsilence_log(x, "SCAN: segment %.2f to %.2f ms was silent. skipping...", (double)b_start * 1000.0 / sr, (double)b_end * 1000.0 / sr);
                x->scan_from = b_end;
                if (x->scan_from >= total_frames) {
                    x->scanner_trigger = 0;
                }
            }
            critical_exit(x->lock);
        } else {
            systhread_sleep(2);
        }
    }
    return NULL;
}

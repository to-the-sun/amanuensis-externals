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
    t_buffer_ref *scan_ref;

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

    long long pending_bar_start;
    long long pending_bar_end;
    int pending_bar_ready;

    int scanner_trigger;
    long long scan_from;

    int scanner_should_exit;
    t_systhread scanner_thread;
    t_critical lock;

    int zero_bar_mode;
    double last_bar_ms;

    // Sync state
    double bar_ms;
    double last_ramp_ms;
    int sync_waiting;
    int next_bar_needs_sync;
    int is_repeating;

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
        dsp_setup((t_pxobject *)x, 3);

        // Create outlets from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);           // Index 3
        x->playhead_outlet = outlet_new((t_object *)x, "signal");  // Index 2
        outlet_new((t_object *)x, "signal");                       // Index 1 (Right)
        outlet_new((t_object *)x, "signal");                       // Index 0 (Left)

        x->play_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->bar_ref = buffer_ref_new((t_object *)x, gensym("bar"));
        x->scan_ref = buffer_ref_new((t_object *)x, _sym_nothing);

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

        x->pending_bar_start = -1;
        x->pending_bar_end = -1;
        x->pending_bar_ready = 0;

        x->scanner_trigger = 0;
        x->scan_from = 0;
        x->zero_bar_mode = 0;
        x->last_bar_ms = -1;

        x->bar_ms = 0.0;
        x->last_ramp_ms = -1.0;
        x->sync_waiting = 0;
        x->next_bar_needs_sync = 0;
        x->is_repeating = 0;

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
    if (x->scan_ref) object_free(x->scan_ref);
}

t_max_err skipsilence_notify(t_skipsilence *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    if (x->play_ref) buffer_ref_notify(x->play_ref, s, msg, sender, data);
    if (x->bar_ref) buffer_ref_notify(x->bar_ref, s, msg, sender, data);
    if (x->scan_ref) buffer_ref_notify(x->scan_ref, s, msg, sender, data);
    return MAX_ERR_NONE;
}

void skipsilence_assist(t_skipsilence *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1 (messages): play [buffer_name]"); break;
            case 1: sprintf(s, "Inlet 2 (int): Play Mode (0: once, 1: repeat, 2: stop)"); break;
            case 2: sprintf(s, "Inlet 3 (signal): Time Ramp (milliseconds)"); break;
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
    long inlet = x->b_obj.z_in;
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
    long inlet = x->b_obj.z_in;
    if (inlet != 0) return;

    critical_enter(x->lock);
    if (x->playing) {
        x->pending_play_sym = s;
        x->pending_bar_ready = 0;
        x->scanner_trigger = 1;
        x->scan_from = 0;
        skipsilence_log(x, "PLAY: queuing playback of buffer '%s' (synchronized transition while playing current buffer)", s->s_name);
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

        x->sync_waiting = 0;
        x->next_bar_needs_sync = 0;
        x->is_repeating = 0;
        x->pending_play_sym = NULL;

        skipsilence_log(x, "PLAY: searching for audible segments in buffer '%s' (will require sync)", s->s_name);
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
        double ramp = ins[2][i];
        int sync_event = 0;

        if (x->bar_ms > 0) {
            if (ramp == 0.0) {
                sync_event = 1;
            } else if (x->last_ramp_ms >= 0) {
                if (ramp < x->last_ramp_ms) {
                    // Ramp looped
                    sync_event = 1;
                } else {
                    double prev_bars = floor(x->last_ramp_ms / x->bar_ms);
                    double curr_bars = floor(ramp / x->bar_ms);
                    if (curr_bars > prev_bars) {
                        sync_event = 1;
                    }
                }
            }
        } else {
            sync_event = 1; // Always sync if bar_length is not set
        }
        x->last_ramp_ms = ramp;

        if (sync_event) {
            if (x->playing && x->pending_bar_ready) {
                double ms_s = (double)x->pending_bar_start * 1000.0 / sr;
                double ms_e = (double)x->pending_bar_end * 1000.0 / sr;
                skipsilence_log(x, "PERFORM: synchronized switch to pending buffer '%s' at %.2f ms", x->pending_play_sym->s_name, ms_s);
                buffer_ref_set(x->play_ref, x->pending_play_sym);
                x->current_bar_start = x->pending_bar_start;
                x->current_bar_end = x->pending_bar_end;
                x->playhead = (double)x->current_bar_start;
                x->pending_play_sym = NULL;
                x->pending_bar_ready = 0;
                x->next_bar_ready = 0;
                x->scan_from = x->current_bar_end;
                x->scanner_trigger = 1;
                x->sync_waiting = 0;
                x->next_bar_needs_sync = 0;
            } else if (x->sync_waiting) {
                if (x->next_bar_ready) {
                    double ms_s = (double)x->next_bar_start * 1000.0 / sr;
                    double ms_e = (double)x->next_bar_end * 1000.0 / sr;
                    skipsilence_log(x, "PERFORM: sync achieved (ramp: %.2f), switching to bar %.2f to %.2f ms", ramp, ms_s, ms_e);
                    x->current_bar_start = x->next_bar_start;
                    x->current_bar_end = x->next_bar_end;
                    x->playhead = (double)x->current_bar_start;
                    x->next_bar_ready = 0;
                    x->scan_from = x->current_bar_end;
                    x->scanner_trigger = 1;
                    x->playing = 1;
                    x->sync_waiting = 0;
                    x->next_bar_needs_sync = 0;
                } else if (!x->playing) {
                    x->playing = 1;
                    x->sync_waiting = 0;
                }
            }
        }

        if (x->playing && samples && !x->sync_waiting) {
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
                    if (x->play_mode == 2) {
                        skipsilence_log(x, "PERFORM: enacting deferred stop");
                        x->playing = 0;
                        x->next_bar_ready = 0;
                        x->scanner_trigger = 0;
                    } else if (x->next_bar_ready) {
                        double ms_s = (double)x->next_bar_start * 1000.0 / sr;
                        double ms_e = (double)x->next_bar_end * 1000.0 / sr;
                        if (x->next_bar_needs_sync) {
                            skipsilence_log(x, "PERFORM: next bar ready but needs sync, waiting (ramp: %.2f)", ramp);
                            x->sync_waiting = 1;
                            x->playhead = (double)x->current_bar_end; // Hold at end (silence) while waiting
                        } else {
                            skipsilence_log(x, "PERFORM: switching to next bar %.2f to %.2f ms", ms_s, ms_e);
                            x->current_bar_start = x->next_bar_start;
                            x->current_bar_end = x->next_bar_end;
                            x->playhead = (double)x->current_bar_start;
                            x->next_bar_ready = 0;
                            x->scan_from = x->current_bar_end;
                            x->scanner_trigger = 1;
                        }
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
                            // Repeat mode: hold at end (silence) while waiting for scanner or sync
                            x->playhead = (double)x->current_bar_end;
                            if (x->next_bar_ready && x->next_bar_needs_sync) {
                                x->sync_waiting = 1;
                            }
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

        critical_enter(x->lock);
        t_symbol *target_buf_sym = x->pending_play_sym;
        critical_exit(x->lock);

        t_buffer_ref *current_scan_ref = x->play_ref;
        if (target_buf_sym) {
            buffer_ref_set(x->scan_ref, target_buf_sym);
            current_scan_ref = x->scan_ref;
        }
        t_buffer_obj *play_b = buffer_ref_getobject(current_scan_ref);

        critical_enter(x->lock);
        int bar_length_changed = (bar_ms != x->last_bar_ms);
        x->last_bar_ms = bar_ms;
        x->bar_ms = bar_ms; // Update sync bar_ms
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
                        x->sync_waiting = 0; // immediate start in zero-bar mode
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
        int is_pending_scan = 0;
        critical_enter(x->lock);
        is_pending_scan = (x->pending_play_sym != NULL && x->playing);
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
        if (is_pending_scan) {
            trigger = x->scanner_trigger && !x->pending_bar_ready;
        } else {
            trigger = x->scanner_trigger && !x->next_bar_ready;
        }
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
                    x->is_repeating = 1; // Mark that we are looping back
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
                if (is_pending_scan) {
                    x->pending_bar_start = b_start;
                    x->pending_bar_end = b_end;
                    x->pending_bar_ready = 1;
                    x->scanner_trigger = 0;
                    skipsilence_log(x, "SCAN: found first bar in pending buffer at %.2f ms", (double)x->pending_bar_start * 1000.0 / sr);
                } else {
                    x->next_bar_start = b_start;
                    x->next_bar_end = b_end;
                    x->next_bar_ready = 1;
                    x->scanner_trigger = 0;

                    if (!x->playing) {
                    if (x->bar_ms > 0) {
                        x->sync_waiting = 1;
                        x->next_bar_ready = 1;
                        x->scanner_trigger = 0;
                        skipsilence_log(x, "SCAN: found audio, waiting for sync to start playback at %.2f ms", (double)x->next_bar_start * 1000.0 / sr);
                    } else {
                        x->current_bar_start = x->next_bar_start;
                        x->current_bar_end = x->next_bar_end;
                        x->playhead = (double)x->current_bar_start;
                        x->playing = 1;
                        x->sync_waiting = 0;
                        x->next_bar_ready = 0;
                        x->scan_from = x->current_bar_end;
                        x->scanner_trigger = 1;
                        skipsilence_log(x, "SCAN: found audio, starting playback immediately at %.2f to %.2f ms", (double)x->current_bar_start * 1000.0 / sr, (double)x->current_bar_end * 1000.0 / sr);
                    }
                    } else {
                        if (target_buf_sym) {
                            x->next_bar_ready = 1;
                            x->scanner_trigger = 0;
                            skipsilence_log(x, "SCAN: found next audio bar in new buffer '%s' at %.2f ms, will require sync", target_buf_sym->s_name, (double)x->next_bar_start * 1000.0 / sr);
                        } else if (x->is_repeating) {
                            x->next_bar_needs_sync = 1;
                            x->is_repeating = 0;
                            skipsilence_log(x, "SCAN: found next audio bar after repeat at %.2f ms, will require sync", (double)x->next_bar_start * 1000.0 / sr);
                        } else {
                            skipsilence_log(x, "SCAN: found next audio bar at %.2f to %.2f ms", (double)x->next_bar_start * 1000.0 / sr, (double)x->next_bar_end * 1000.0 / sr);
                        }
                    }
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

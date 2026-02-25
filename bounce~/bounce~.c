#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include "../shared/crossfade.h"
#include <string.h>
#include <math.h>

#define BOUNCE_LOG_QUEUE_SIZE 64

typedef struct _bounce {
    t_object b_obj;
    t_symbol *poly_prefix;
    t_symbol *dest_name;
    t_buffer_ref *poly_ref; // Canary for polybuffer (prefix.1)
    t_buffer_ref *dest_ref;
    t_buffer_ref *stats_ref;
    long log;
    void *log_outlet;
    void *bang_outlet;
    long poly_found;
    long dest_found;
    long stats_found;
    long poly_error_sent;
    long dest_error_sent;
    long stats_error_sent;
    double normalize_to;
    double low_ms;
    double high_ms;
    t_systhread thread;
    void *qelem;
    long busy;
    long async_attr;
    double ms_end;
    t_buffer_obj *stem_objs[1024];
    int stem_count;
    t_buffer_obj *dest_obj;

    char log_queue[BOUNCE_LOG_QUEUE_SIZE][1024];
    int log_head;
    int log_tail;
    t_critical log_lock;
    void *log_qelem;
} t_bounce;

void *bounce_new(t_symbol *s, long argc, t_atom *argv);
void bounce_free(t_bounce *x);
void bounce_assist(t_bounce *x, void *b, long m, long a, char *s);
void bounce_bang(t_bounce *x);
void *bounce_worker(t_bounce *x);
void bounce_qfn(t_bounce *x);
void bounce_log_qfn(t_bounce *x);
void bounce_check_attachments(t_bounce *x, long report_error);
void bounce_log(t_bounce *x, const char *fmt, ...);

static t_class *bounce_class;

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("bounce~", (method)bounce_new, (method)bounce_free, sizeof(t_bounce), 0L, A_GIMME, 0);

    class_addmethod(c, (method)bounce_bang, "bang", 0);
    class_addmethod(c, (method)bounce_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_bounce, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    CLASS_ATTR_DOUBLE(c, "normalize", 0, t_bounce, normalize_to);
    CLASS_ATTR_LABEL(c, "normalize", 0, "Normalization Target Amplitude");
    CLASS_ATTR_DEFAULT(c, "normalize", 0, "0");

    CLASS_ATTR_DOUBLE(c, "low", 0, t_bounce, low_ms);
    CLASS_ATTR_LABEL(c, "low", 0, "Low Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "low", 0, "22.653");

    CLASS_ATTR_DOUBLE(c, "high", 0, t_bounce, high_ms);
    CLASS_ATTR_LABEL(c, "high", 0, "High Limit (ms)");
    CLASS_ATTR_DEFAULT(c, "high", 0, "4999.0");

    CLASS_ATTR_LONG(c, "async", 0, t_bounce, async_attr);
    CLASS_ATTR_STYLE_LABEL(c, "async", 0, "onoff", "Asynchronous Execution");
    CLASS_ATTR_DEFAULT(c, "async", 0, "0");

    class_register(CLASS_BOX, c);
    bounce_class = c;
}

void bounce_log(t_bounce *x, const char *fmt, ...) {
    if (!x || !x->log) return;
    va_list args;
    va_start(args, fmt);
    if (systhread_ismainthread() || systhread_istimerthread()) {
        vcommon_log(x->log_outlet, x->log, "bounce~", fmt, args);
    } else {
        char buf[1024];
        vsnprintf(buf, 1024, fmt, args);
        critical_enter(x->log_lock);
        int next_tail = (x->log_tail + 1) % BOUNCE_LOG_QUEUE_SIZE;
        if (next_tail != x->log_head) {
            strncpy(x->log_queue[x->log_tail], buf, 1024);
            x->log_tail = next_tail;
        }
        critical_exit(x->log_lock);
        qelem_set(x->log_qelem);
    }
    va_end(args);
}

void bounce_check_attachments(t_bounce *x, long report_error) {
    // Polybuffer check (via prefix.1)
    if (x->poly_prefix != _sym_nothing) {
        t_buffer_obj *b = buffer_ref_getobject(x->poly_ref);
        if (!b) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            buffer_ref_set(x->poly_ref, _sym_nothing);
            buffer_ref_set(x->poly_ref, gensym(t1name));
            b = buffer_ref_getobject(x->poly_ref);
        }
        if (b) {
            x->poly_error_sent = 0; // Reset flag when buffer is successfully found
            if (!x->poly_found) {
                bounce_log(x, "successfully found polybuffer~ '%s' (via %s.1)", x->poly_prefix->s_name, x->poly_prefix->s_name);
                x->poly_found = 1;
            }
        } else {
            if (x->poly_found) x->poly_found = 0;
            if (report_error && !x->poly_error_sent) {
                object_warn((t_object *)x, "polybuffer~ '%s' not found (could not find %s.1)", x->poly_prefix->s_name, x->poly_prefix->s_name);
                x->poly_error_sent = 1;
            }
        }
    }

    // Destination check
    if (x->dest_name != _sym_nothing) {
        t_buffer_obj *b = buffer_ref_getobject(x->dest_ref);
        if (!b) {
            buffer_ref_set(x->dest_ref, _sym_nothing);
            buffer_ref_set(x->dest_ref, x->dest_name);
            b = buffer_ref_getobject(x->dest_ref);
        }
        if (b) {
            x->dest_error_sent = 0; // Reset flag when buffer is successfully found
            if (!x->dest_found) {
                bounce_log(x, "successfully found destination buffer~ '%s'", x->dest_name->s_name);
                x->dest_found = 1;
            }
        } else {
            if (x->dest_found) x->dest_found = 0;
            if (report_error && !x->dest_error_sent) {
                object_error((t_object *)x, "destination buffer~ '%s' not found", x->dest_name->s_name);
                x->dest_error_sent = 1;
            }
        }
    }

    // Stats check
    t_buffer_obj *b_stats = buffer_ref_getobject(x->stats_ref);
    if (!b_stats) {
        buffer_ref_set(x->stats_ref, _sym_nothing);
        buffer_ref_set(x->stats_ref, gensym("stats"));
        b_stats = buffer_ref_getobject(x->stats_ref);
    }
    if (b_stats) {
        x->stats_error_sent = 0;
        if (!x->stats_found) {
            bounce_log(x, "successfully found stats buffer~");
            x->stats_found = 1;
        }
    } else {
        if (x->stats_found) x->stats_found = 0;
        if (report_error && !x->stats_error_sent) {
            object_error((t_object *)x, "stats buffer~ not found");
            x->stats_error_sent = 1;
        }
    }
}

void *bounce_new(t_symbol *s, long argc, t_atom *argv) {
    t_bounce *x = (t_bounce *)object_alloc(bounce_class);

    if (x) {
        x->poly_prefix = _sym_nothing;
        x->dest_name = _sym_nothing;
        x->log = 0;
        x->poly_found = 0;
        x->dest_found = 0;
        x->stats_found = 0;
        x->poly_error_sent = 0;
        x->dest_error_sent = 0;
        x->stats_error_sent = 0;
        x->normalize_to = 0.0;
        x->low_ms = 22.653;
        x->high_ms = 4999.0;
        x->thread = NULL;
        x->qelem = qelem_new(x, (method)bounce_qfn);
        x->busy = 0;
        x->async_attr = 0;

        x->log_head = 0;
        x->log_tail = 0;
        critical_new(&x->log_lock);
        x->log_qelem = qelem_new(x, (method)bounce_log_qfn);

        // Arg 1: Polybuffer prefix
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->poly_prefix = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory polybuffer~ name argument");
        }

        // Arg 2: Destination buffer name
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->dest_name = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory destination buffer~ name argument");
        }

        attr_args_process(x, argc, argv);

        // Outlets (Right-to-Left)
        if (x->log) {
            x->log_outlet = outlet_new((t_object *)x, NULL);
        } else {
            x->log_outlet = NULL;
        }
        x->bang_outlet = outlet_new((t_object *)x, NULL);

        // Init buffer refs
        if (x->poly_prefix != _sym_nothing) {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            x->poly_ref = buffer_ref_new((t_object *)x, gensym(t1name));
        } else {
            x->poly_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        }
        x->dest_ref = buffer_ref_new((t_object *)x, x->dest_name);
        x->stats_ref = buffer_ref_new((t_object *)x, gensym("stats"));

        // Initial search
        bounce_check_attachments(x, 1);
    }
    return x;
}

void bounce_free(t_bounce *x) {
    if (x->thread) {
        systhread_join(x->thread, NULL);
    }
    if (x->qelem) {
        qelem_free(x->qelem);
    }
    if (x->log_qelem) {
        qelem_free(x->log_qelem);
    }
    critical_free(x->log_lock);

    if (x->poly_ref) object_free(x->poly_ref);
    if (x->dest_ref) object_free(x->dest_ref);
    if (x->stats_ref) object_free(x->stats_ref);
}

void bounce_assist(t_bounce *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (bang) start bounce, (low/high) set fade limits");
    } else {
        if (a == 0) {
            sprintf(s, "Outlet 1: (bang) finished");
        } else {
            sprintf(s, "Outlet 2: (anything) Logging Outlet");
        }
    }
}

void bounce_do_work(t_bounce *x) {
    t_buffer_obj *dest_buf = x->dest_obj;

    if (!dest_buf) {
        bounce_log(x, "mandatory buffer(s) missing during bounce");
        return;
    }

    double ms_end = x->ms_end;
    long n_frames_dest = buffer_getframecount(dest_buf);
    long n_chans_dest = buffer_getchannelcount(dest_buf);
    double sr_dest = buffer_getsamplerate(dest_buf);
    if (sr_dest <= 0) sr_dest = 44100.0;

    long limit_dest = (long)round(ms_end * sr_dest / 1000.0);
    if (limit_dest > n_frames_dest) limit_dest = n_frames_dest;
    if (limit_dest < 0) limit_dest = 0;

    bounce_log(x, "Beginning bounce: summing %d stems into '%s' (%.2f ms, %ld frames)", x->stem_count, x->dest_name->s_name, ms_end, limit_dest);

    float *samples_dest = NULL;
    for (int retry = 0; retry < 10; retry++) {
        samples_dest = buffer_locksamples(dest_buf);
        if (samples_dest) break;
        systhread_sleep(1);
    }

    if (!samples_dest) {
        bounce_log(x, "Error: could not lock destination buffer '%s'", x->dest_name->s_name);
        return;
    }

    buffer_edit_begin(dest_buf);

    // Clear destination (up to limit)
    bounce_log(x, "Clearing destination buffer...");
    memset(samples_dest, 0, limit_dest * n_chans_dest * sizeof(float));

    bounce_log(x, "Processing %d stems...", x->stem_count);
    for (int i = 0; i < x->stem_count; i++) {
        t_buffer_obj *src_buf = x->stem_objs[i];
        if (!src_buf) continue;

        bounce_log(x, "Stem %d/%d: starting processing", i + 1, x->stem_count);

        long n_frames_src = buffer_getframecount(src_buf);
        long n_chans_src = buffer_getchannelcount(src_buf);
        double sr_src = buffer_getsamplerate(src_buf);
        if (sr_src <= 0) sr_src = 44100.0;

        long limit_src = (long)round(ms_end * sr_src / 1000.0);
        if (limit_src > n_frames_src) limit_src = n_frames_src;
        if (limit_src < 0) limit_src = 0;

        float *samples_src = NULL;
        for (int retry = 0; retry < 10; retry++) {
            samples_src = buffer_locksamples(src_buf);
            if (samples_src) break;
            systhread_sleep(1);
        }

        if (!samples_src) {
            bounce_log(x, "Stem %d/%d: Error - could not lock buffer", i + 1, x->stem_count);
            continue;
        }

        // 1. End point is defined by stats buffer
        long last_audio_frame = limit_src - 1;

        if (last_audio_frame < 0) {
            bounce_log(x, "Stem %d/%d: skipping (end point before start)", i + 1, x->stem_count);
            buffer_unlocksamples(src_buf);
            continue;
        }

        // 2. Normalize if needed (up to limit_src)
        double max_abs = 0.0;
        for (long f = 0; f < limit_src; f++) {
            for (long c = 0; c < n_chans_src; c++) {
                double a = fabs((double)samples_src[f * n_chans_src + c]);
                if (a > max_abs) max_abs = a;
            }
        }

        if (max_abs > 1.0) {
            bounce_log(x, "Stem %d/%d: peak %.4f exceeds 1.0, normalizing stem buffer", i + 1, x->stem_count, max_abs);
            // Destructive normalization of stems
            // We are already inside global critical section
            buffer_edit_begin(src_buf);
            double scale = 0.9999999 / max_abs;
            for (long f = 0; f < limit_src; f++) {
                for (long c = 0; c < n_chans_src; c++) {
                    samples_src[f * n_chans_src + c] *= (float)scale;
                }
            }
            buffer_edit_end(src_buf, 1);
            buffer_setdirty(src_buf);
        }

        // 3. Add to destination with fades
        long fade_out_duration_frames = (long)round(x->high_ms * sr_src / 1000.0);
        long fade_out_start_frame = last_audio_frame - fade_out_duration_frames + 1;
        if (fade_out_start_frame < 0) fade_out_start_frame = 0;

        bounce_log(x, "Stem %d/%d: summing into destination with fades (fade-out starts at frame %ld)", i + 1, x->stem_count, fade_out_start_frame);

        t_ramp_state ramp;
        ramp_init(&ramp, sr_dest, x->high_ms);
        long long elapsed = 0;
        int fade_in_triggered = 0;
        int fade_out_triggered = 0;

        for (long f_dest = 0; f_dest < limit_dest; f_dest++) {
            if (limit_dest > 1000000 && f_dest > 0 && f_dest % (limit_dest / 4) == 0) {
                bounce_log(x, "Stem %d/%d: summing... %d%%", i + 1, x->stem_count, (int)(100 * f_dest / limit_dest));
            }
            long f_src = (long)round((double)f_dest * sr_src / sr_dest);
            if (f_src >= 0 && f_src < limit_src) {
                double movement = 0.0;
                if (!fade_in_triggered) {
                    movement = -1.0;
                    fade_in_triggered = 1;
                } else if (!fade_out_triggered && f_src >= fade_out_start_frame) {
                    movement = 1.0;
                    fade_out_triggered = 1;
                }

                double max_abs_src = 0.0;
                for (long c = 0; c < n_chans_src; c++) {
                    double a = fabs((double)samples_src[f_src * n_chans_src + c]);
                    if (a > max_abs_src) max_abs_src = a;
                }

                double fade_factor;
                ramp_process(&ramp, max_abs_src, movement, elapsed, sr_dest, x->low_ms, x->high_ms, &fade_factor);

                samples_dest[f_dest * n_chans_dest] += (float)((double)samples_src[f_src * n_chans_src] * fade_factor);
                elapsed++;
            }
        }

        bounce_log(x, "Stem %d/%d: done", i + 1, x->stem_count);

        buffer_unlocksamples(src_buf);
    }

    bounce_log(x, "Summation complete. Duplicating to %ld channels.", n_chans_dest);

    // 4. Duplicate channel 0 to all other channels (up to limit_dest)
    if (n_chans_dest > 1) {
        for (long f = 0; f < limit_dest; f++) {
            float val = samples_dest[f * n_chans_dest];
            for (long c = 1; c < n_chans_dest; c++) {
                samples_dest[f * n_chans_dest + c] = val;
            }
        }
    }

    // 5. Final normalization of product buffer if requested (up to limit_dest)
    if (x->normalize_to != 0.0) {
        bounce_log(x, "Applying final normalization (target: %.2f)...", x->normalize_to);
        double product_max = 0.0;
        long total_samples = limit_dest * n_chans_dest;
        for (long i = 0; i < total_samples; i++) {
            if (total_samples > 1000000 && i > 0 && i % (total_samples / 4) == 0) {
                bounce_log(x, "Final normalization: analyzing... %d%%", (int)(100 * i / total_samples));
            }
            double a = fabs((double)samples_dest[i]);
            if (a > product_max) product_max = a;
        }

        if (product_max > 0.0) {
            double target = fabs(x->normalize_to);
            double scale = target / product_max;
            for (long i = 0; i < total_samples; i++) {
                if (total_samples > 1000000 && i > 0 && i % (total_samples / 4) == 0) {
                    bounce_log(x, "Final normalization: applying... %d%%", (int)(100 * i / total_samples));
                }
                samples_dest[i] *= (float)scale;
            }
            bounce_log(x, "Final product normalized (peak was %.4f)", product_max);
        }
    }

    buffer_edit_end(dest_buf, 1);
    buffer_unlocksamples(dest_buf);
    buffer_setdirty(dest_buf);
}

void *bounce_worker(t_bounce *x) {
    bounce_do_work(x);
    qelem_set(x->qelem);
    return NULL;
}

void bounce_bang(t_bounce *x) {
    // 1. Retrieve the length from the stats buffer~ very first thing synchronously
    double ms_end = 0.0;
    t_buffer_obj *stats_buf = buffer_ref_getobject(x->stats_ref);
    if (!stats_buf) {
        buffer_ref_set(x->stats_ref, _sym_nothing);
        buffer_ref_set(x->stats_ref, gensym("stats"));
        stats_buf = buffer_ref_getobject(x->stats_ref);
    }
    if (stats_buf) {
        float *samples_stats = buffer_locksamples(stats_buf);
        if (samples_stats) {
            if (buffer_getframecount(stats_buf) > 1) {
                ms_end = (double)samples_stats[1];
            }
            buffer_unlocksamples(stats_buf);
        }
    }
    x->ms_end = ms_end;

    // 2. Check if already busy
    if (x->busy) {
        bounce_log(x, "bounce process already in progress, ignoring bang");
        return;
    }

    // 3. Normal setup
    bounce_log(x, "starting bounce process");
    bounce_check_attachments(x, 1);

    if (!x->poly_found || !x->dest_found || !x->stats_found) {
        bounce_log(x, "mandatory buffer(s) missing during bounce");
        return;
    }

    // Pre-collect pointers in main thread
    x->dest_obj = buffer_ref_getobject(x->dest_ref);
    x->stem_count = 0;
    t_buffer_ref *src_ref = buffer_ref_new((t_object *)x, _sym_nothing);
    for (int i = 1; i <= 1024; i++) {
        char bufname[256];
        snprintf(bufname, 256, "%s.%d", x->poly_prefix->s_name, i);
        buffer_ref_set(src_ref, gensym(bufname));
        t_buffer_obj *b = buffer_ref_getobject(src_ref);
        if (!b) break;
        x->stem_objs[x->stem_count++] = b;
    }
    object_free(src_ref);

    if (x->async_attr) {
        x->busy = 1;
        bounce_log(x, "Spawning background thread for asynchronous bounce.");
        systhread_create((method)bounce_worker, x, 0, 0, 0, &x->thread);
    } else {
        bounce_do_work(x);
        bounce_log(x, "Bounce process finished successfully.");
        outlet_bang(x->bang_outlet);
    }
}

void bounce_qfn(t_bounce *x) {
    if (x->thread) {
        systhread_join(x->thread, NULL);
        x->thread = NULL;
    }
    bounce_log(x, "Bounce process finished successfully.");
    outlet_bang(x->bang_outlet);
    x->busy = 0;
}

void bounce_log_qfn(t_bounce *x) {
    char msg[1024];
    while (1) {
        critical_enter(x->log_lock);
        if (x->log_head == x->log_tail) {
            critical_exit(x->log_lock);
            break;
        }
        strncpy(msg, x->log_queue[x->log_head], 1024);
        x->log_head = (x->log_head + 1) % BOUNCE_LOG_QUEUE_SIZE;
        critical_exit(x->log_lock);

        common_log(x->log_outlet, x->log, "bounce~", "%s", msg);
    }
}

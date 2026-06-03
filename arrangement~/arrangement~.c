#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include "../shared/crossfade.h"
#include <math.h>
#include <string.h>

typedef struct _arrangement {
    t_pxobject t_obj;

    // Buffers
    t_buffer_ref *stem_ref;
    t_buffer_ref *palette_ref;
    t_buffer_ref *product_ref;
    t_buffer_ref *comp_ref;
    t_buffer_ref *stats_ref;
    t_buffer_ref *following_ref;
    t_buffer_ref *bar_ref;

    // Params
    double tracks;
    double track;
    double gain1;
    double gain2;
    double gain;
    double offset;
    double rt_exporting;
    double ready;
    double auditioning;
    double lead;
    double interpolation;

    // Histories
    double leads;
    double from;
    double to;
    double go;
    double extended_t;
    double delayed_go;
    double loops;
    double zoom1;
    double zoom2;
    double t;
    double debug;
    double domain;
    double restart;

    // Attributes
    long mc_channel;

    // DSP internal
    long long elapsed_counter;
    t_crossfade_state xf_audition;
    t_crossfade_state xf_interpolation;

    // Change detection
    double last_from;
    double last_to;
    double last_out6_busy;
    double last_fading_busy;
    double last_t;

} t_arrangement;

typedef struct _arrangement_buffer_info {
    float *samples;
    long long framecount;
    long chancount;
} t_arrangement_buffer_info;

// Prototypes
void *arrangement_new(t_symbol *s, long argc, t_atom *argv);
void arrangement_free(t_arrangement *x);
void arrangement_dsp64(t_arrangement *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void arrangement_perform64(t_arrangement *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void arrangement_assist(t_arrangement *x, void *b, long m, long a, char *s);
t_max_err arrangement_notify(t_arrangement *x, t_symbol *s, t_symbol *msg, void *sender, void *data);

// Buffer messages
void arrangement_stem(t_arrangement *x, t_symbol *s);
void arrangement_palette(t_arrangement *x, t_symbol *s);
void arrangement_product(t_arrangement *x, t_symbol *s);
void arrangement_comp(t_arrangement *x, t_symbol *s);
void arrangement_stats(t_arrangement *x, t_symbol *s);
void arrangement_following(t_arrangement *x, t_symbol *s);
void arrangement_bar(t_arrangement *x, t_symbol *s);

// Param/History messages
void arrangement_tracks(t_arrangement *x, double v);
void arrangement_track(t_arrangement *x, double v);
void arrangement_gain1(t_arrangement *x, double v);
void arrangement_gain2(t_arrangement *x, double v);
void arrangement_gain(t_arrangement *x, double v);
void arrangement_offset(t_arrangement *x, double v);
void arrangement_rt_exporting(t_arrangement *x, double v);
void arrangement_ready(t_arrangement *x, double v);
void arrangement_auditioning(t_arrangement *x, double v);
void arrangement_lead(t_arrangement *x, double v);
void arrangement_interpolation(t_arrangement *x, double v);

void arrangement_leads(t_arrangement *x, double v);
void arrangement_from(t_arrangement *x, double v);
void arrangement_to(t_arrangement *x, double v);
void arrangement_go(t_arrangement *x, double v);
void arrangement_extended_t(t_arrangement *x, double v);
void arrangement_delayed_go(t_arrangement *x, double v);
void arrangement_loops(t_arrangement *x, double v);
void arrangement_zoom1(t_arrangement *x, double v);
void arrangement_zoom2(t_arrangement *x, double v);
void arrangement_t_msg(t_arrangement *x, double v);
void arrangement_debug(t_arrangement *x, double v);
void arrangement_domain(t_arrangement *x, double v);
void arrangement_restart(t_arrangement *x, double v);

static t_class *arrangement_class;

// Helpers
static inline double mstosamps(double ms, double sr) {
    return ms * sr / 1000.0;
}

static inline double gen_round(double v, double step, const char *mode) {
    if (step <= 0) return round(v);
    if (strcmp(mode, "floor") == 0) return floor(v / step) * step;
    if (strcmp(mode, "ceil") == 0) return ceil(v / step) * step;
    return round(v / step) * step;
}

static t_arrangement_buffer_info lock_buffer(t_buffer_ref *ref) {
    t_arrangement_buffer_info info = {NULL, 0, 0};
    t_buffer_obj *obj = buffer_ref_getobject(ref);
    if (obj) {
        info.samples = buffer_locksamples(obj);
        if (info.samples) {
            info.framecount = buffer_getframecount(obj);
            info.chancount = buffer_getchannelcount(obj);
        }
    }
    return info;
}

static void unlock_buffer(t_buffer_ref *ref) {
    t_buffer_obj *obj = buffer_ref_getobject(ref);
    if (obj) buffer_unlocksamples(obj);
}

static float peek_info(t_arrangement_buffer_info info, double index) {
    if (!info.samples) return 0.0f;
    long long idx = (long long)index;
    if (idx >= 0 && idx < info.framecount) {
        return info.samples[idx * info.chancount];
    }
    return 0.0f;
}

static void poke_info(t_arrangement_buffer_info info, double index, float value) {
    if (!info.samples) return;
    long long idx = (long long)index;
    if (idx >= 0 && idx < info.framecount) {
        info.samples[idx * info.chancount] = value;
    }
}

void ext_main(void *r) {
    t_class *c = class_new("arrangement~", (method)arrangement_new, (method)arrangement_free, sizeof(t_arrangement), 0L, A_GIMME, 0);

    class_addmethod(c, (method)arrangement_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)arrangement_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)arrangement_notify, "notify", A_CANT, 0);

    // Buffer methods
    class_addmethod(c, (method)arrangement_stem, "stem", A_SYM, 0);
    class_addmethod(c, (method)arrangement_palette, "palette", A_SYM, 0);
    class_addmethod(c, (method)arrangement_product, "product", A_SYM, 0);
    class_addmethod(c, (method)arrangement_comp, "comp", A_SYM, 0);
    class_addmethod(c, (method)arrangement_stats, "stats", A_SYM, 0);
    class_addmethod(c, (method)arrangement_following, "following", A_SYM, 0);
    class_addmethod(c, (method)arrangement_bar, "bar", A_SYM, 0);

    // Param methods
    class_addmethod(c, (method)arrangement_tracks, "tracks", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_track, "track", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_gain1, "gain1", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_gain2, "gain2", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_gain, "gain", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_offset, "offset", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_rt_exporting, "rt_exporting", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_ready, "ready", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_auditioning, "auditioning", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_lead, "lead", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_interpolation, "interpolation", A_FLOAT, 0);

    // History methods
    class_addmethod(c, (method)arrangement_leads, "leads", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_from, "from", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_to, "to", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_go, "go", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_extended_t, "extended_t", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_delayed_go, "delayed_go", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_loops, "loops", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_zoom1, "zoom1", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_zoom2, "zoom2", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_t_msg, "t", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_debug, "debug", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_domain, "domain", A_FLOAT, 0);
    class_addmethod(c, (method)arrangement_restart, "restart", A_FLOAT, 0);

    CLASS_ATTR_LONG(c, "track_id", 0, t_arrangement, mc_channel);
    CLASS_ATTR_DEFAULT(c, "track_id", 0, "1");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    arrangement_class = c;
}

void *arrangement_new(t_symbol *s, long argc, t_atom *argv) {
    t_arrangement *x = (t_arrangement *)object_alloc(arrangement_class);
    if (x) {
        dsp_setup((t_pxobject *)x, 1);

        // Outlets: 7 signal outlets
        for (int i = 0; i < 7; i++) {
            outlet_new((t_object *)x, "signal");
        }

        x->stem_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->palette_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->product_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->comp_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->stats_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->following_ref = buffer_ref_new((t_object *)x, _sym_nothing);
        x->bar_ref = buffer_ref_new((t_object *)x, _sym_nothing);

        // Default values
        x->tracks = 0;
        x->track = 0;
        x->gain1 = 1;
        x->gain2 = 1;
        x->gain = 1;
        x->offset = 0;
        x->rt_exporting = 0;
        x->ready = 0;
        x->auditioning = 0;
        x->lead = 44;
        x->interpolation = 0;

        x->leads = 44100;
        x->from = 0;
        x->to = 0;
        x->go = 0;
        x->extended_t = 0;
        x->delayed_go = 0;
        x->loops = 0;
        x->zoom1 = 0;
        x->zoom2 = 1;
        x->t = 0;
        x->debug = 0;
        x->domain = 0;
        x->restart = 0;

        x->mc_channel = 1;
        x->elapsed_counter = 0;

        x->last_from = 0;
        x->last_to = 0;
        x->last_out6_busy = 0;
        x->last_fading_busy = 0;
        x->last_t = 0;

        attr_args_process(x, argc, argv);
    }
    return x;
}

void arrangement_free(t_arrangement *x) {
    dsp_free((t_pxobject *)x);
    object_free(x->stem_ref);
    object_free(x->palette_ref);
    object_free(x->product_ref);
    object_free(x->comp_ref);
    object_free(x->stats_ref);
    object_free(x->following_ref);
    object_free(x->bar_ref);
}

void arrangement_assist(t_arrangement *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet %ld: Signal/Messages", a + 1);
    } else {
        switch (a) {
            case 0: sprintf(s, "(signal) out1: result"); break;
            case 1: sprintf(s, "(signal) out2: t"); break;
            case 2: sprintf(s, "(signal) out3: zoom1"); break;
            case 3: sprintf(s, "(signal) out4: crossfade mix (mix1)"); break;
            case 4: sprintf(s, "(signal) out5: zoom2"); break;
            case 5: sprintf(s, "(signal) out6: t delta < 0"); break;
            case 6: sprintf(s, "(signal) out7: domain"); break;
        }
    }
}

t_max_err arrangement_notify(t_arrangement *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    buffer_ref_notify(x->stem_ref, s, msg, sender, data);
    buffer_ref_notify(x->palette_ref, s, msg, sender, data);
    buffer_ref_notify(x->product_ref, s, msg, sender, data);
    buffer_ref_notify(x->comp_ref, s, msg, sender, data);
    buffer_ref_notify(x->stats_ref, s, msg, sender, data);
    buffer_ref_notify(x->following_ref, s, msg, sender, data);
    buffer_ref_notify(x->bar_ref, s, msg, sender, data);
    return MAX_ERR_NONE;
}

// Implement methods
void arrangement_stem(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->stem_ref, s); }
void arrangement_palette(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->palette_ref, s); }
void arrangement_product(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->product_ref, s); }
void arrangement_comp(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->comp_ref, s); }
void arrangement_stats(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->stats_ref, s); }
void arrangement_following(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->following_ref, s); }
void arrangement_bar(t_arrangement *x, t_symbol *s) { buffer_ref_set(x->bar_ref, s); }

void arrangement_tracks(t_arrangement *x, double v) { x->tracks = v; }
void arrangement_track(t_arrangement *x, double v) { x->track = v; }
void arrangement_gain1(t_arrangement *x, double v) { x->gain1 = v; }
void arrangement_gain2(t_arrangement *x, double v) { x->gain2 = v; }
void arrangement_gain(t_arrangement *x, double v) { x->gain = v; }
void arrangement_offset(t_arrangement *x, double v) { x->offset = v; }
void arrangement_rt_exporting(t_arrangement *x, double v) { x->rt_exporting = v; }
void arrangement_ready(t_arrangement *x, double v) { x->ready = v; }
void arrangement_auditioning(t_arrangement *x, double v) { x->auditioning = v; }
void arrangement_lead(t_arrangement *x, double v) { x->lead = v; }
void arrangement_interpolation(t_arrangement *x, double v) { x->interpolation = v; }

void arrangement_leads(t_arrangement *x, double v) { x->leads = v; }
void arrangement_from(t_arrangement *x, double v) { x->from = v; }
void arrangement_to(t_arrangement *x, double v) { x->to = v; }
void arrangement_go(t_arrangement *x, double v) { x->go = v; }
void arrangement_extended_t(t_arrangement *x, double v) { x->extended_t = v; }
void arrangement_delayed_go(t_arrangement *x, double v) { x->delayed_go = v; }
void arrangement_loops(t_arrangement *x, double v) { x->loops = v; }
void arrangement_zoom1(t_arrangement *x, double v) { x->zoom1 = v; }
void arrangement_zoom2(t_arrangement *x, double v) { x->zoom2 = v; }
void arrangement_t_msg(t_arrangement *x, double v) { x->t = v; }
void arrangement_debug(t_arrangement *x, double v) { x->debug = v; }
void arrangement_domain(t_arrangement *x, double v) { x->domain = v; }
void arrangement_restart(t_arrangement *x, double v) { x->restart = v; }

void arrangement_dsp64(t_arrangement *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->elapsed_counter = 0;
    crossfade_init(&x->xf_audition, samplerate, 22.653, 4999.0);
    crossfade_init(&x->xf_interpolation, samplerate, 22.653, 4999.0);
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)arrangement_perform64, 0, NULL);
}

void arrangement_perform64(t_arrangement *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *out1 = outs[0];
    double *out2 = outs[1];
    double *out3 = outs[2];
    double *out4 = outs[3];
    double *out5 = outs[4];
    double *out6 = outs[5];
    double *out7 = outs[6];

    double samplerate = sys_getsr();
    if (samplerate <= 0) samplerate = 44100.0;

    t_arrangement_buffer_info stem_info = lock_buffer(x->stem_ref);
    t_arrangement_buffer_info palette_info = lock_buffer(x->palette_ref);
    t_arrangement_buffer_info stats_info = lock_buffer(x->stats_ref);
    t_arrangement_buffer_info bar_info = lock_buffer(x->bar_ref);
    t_arrangement_buffer_info comp_info = lock_buffer(x->comp_ref);

    for (int i = 0; i < sampleframes; i++) {
        double o1 = 0, o2 = 0, o3 = 0, o4 = 0, o5 = 0, o6 = 0, o7 = 0;

        if (x->ready || x->rt_exporting) {
            if (x->domain == 0.0) {
                x->domain = round(mstosamps(peek_info(stats_info, 1), samplerate));
            }
            int follow = (x->to - x->from == x->domain);
            if (follow) {
                x->domain = round(mstosamps(peek_info(stats_info, 1), samplerate));
                x->to = x->domain;
                x->zoom1 = fmax(0.0, x->t - fmax(x->lead, 88200.0));
                x->zoom2 = fmin(x->domain, x->t + fmax(x->lead, 88200.0)) - x->zoom1;
            }
            poke_info(stats_info, 10, x->domain > 0 ? 1.0f : 0.0f);

            double bar_length = mstosamps(peek_info(bar_info, 0), samplerate);
            double start = fmax(0.0, gen_round(x->from - x->lead, bar_length, "floor"));
            double end = fmin(x->domain, gen_round(x->to + x->lead, bar_length, "ceil"));
            // double length = end - start; // Calculated in genexpr but not used as out

            x->t = (double)x->elapsed_counter - x->go + start;
            x->extended_t = (double)x->elapsed_counter - x->delayed_go + start;

            int from_changed = (x->from != x->last_from);
            int to_changed = (x->to != x->last_to);

            if (((from_changed || to_changed) && x->from != 0) || x->t > end || x->restart) {
                x->restart = 0;
                x->go = (double)x->elapsed_counter;
                x->loops += 1.0;
                x->zoom1 = fmax(0.0, start);
                x->zoom2 = fmin(x->domain, end) - x->zoom1;
                if (x->from == x->to) {
                    x->go -= x->from;
                    x->from = 0;
                    x->to = x->domain;
                }
            }
            x->last_from = x->from;
            x->last_to = x->to;

            if (x->track == (double)x->mc_channel && x->auditioning) {
                int on = (x->t > x->from && x->t < x->to);
                double surround = peek_info(stem_info, x->t);
                double diff_to_from = x->to - x->from;
                double m = (diff_to_from != 0) ? (x->gain2 - x->gain1) / diff_to_from : 0;
                double comp_gain = (x->t - x->from) * m + x->gain1;
                double audition = peek_info(palette_info, x->t + x->offset) * comp_gain;

                double mix1, mix2, sum;
                int busy;
                crossfade_process(&x->xf_audition, (double)on, surround, audition, &mix1, &mix2, &sum, &busy);
                o4 = mix1;
                o1 = sum;

                if (!on && ((double)busy - x->last_out6_busy < 0)) { // change(out6) < 0
                    poke_info(comp_info, x->t, 1.0f);
                    if (comp_info.samples) {
                        t_buffer_obj *comp_obj = buffer_ref_getobject(x->comp_ref);
                        if (comp_obj) buffer_setdirty(comp_obj);
                    }
                }
                x->last_out6_busy = (double)busy;
            } else {
                o1 = peek_info(stem_info, x->t);
            }

            if (x->interpolation) {
                double trail = peek_info(stem_info, x->extended_t) * x->gain2;
                double mix1, mix2, sum;
                int busy;
                crossfade_process(&x->xf_interpolation, x->loops, trail, o1, &mix1, &mix2, &sum, &busy);
                o1 = sum;
                if ((double)busy - x->last_fading_busy < 0) { // change(fading) < 0
                    x->delayed_go = x->go;
                }
                x->last_fading_busy = (double)busy;
            }
        }

        if (x->domain <= 0) x->t *= 0; // t *= domain > 0
        o1 *= x->gain;
        o2 = x->t;
        o3 = x->zoom1;
        o5 = x->zoom2;
        o6 = (x->t - x->last_t < 0) ? 1.0 : 0.0; // delta(t) < 0
        o7 = x->domain;

        x->last_t = x->t;

        out1[i] = o1;
        out2[i] = o2;
        out3[i] = o3;
        out4[i] = o4;
        out5[i] = o5;
        out6[i] = o6;
        out7[i] = o7;

        x->elapsed_counter++;
    }

    unlock_buffer(x->stem_ref);
    unlock_buffer(x->palette_ref);
    unlock_buffer(x->stats_ref);
    unlock_buffer(x->bar_ref);
    unlock_buffer(x->comp_ref);
}

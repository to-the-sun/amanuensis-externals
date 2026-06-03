#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include "../shared/crossfade.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

// Forward declarations for methods
void arrangement_stem(void *x, t_symbol *s);
void arrangement_palette(void *x, t_symbol *s);
void arrangement_product(void *x, t_symbol *s);
void arrangement_comp(void *x, t_symbol *s);
void arrangement_stats(void *x, t_symbol *s);
void arrangement_following(void *x, t_symbol *s);
void arrangement_bar(void *x, t_symbol *s);

void arrangement_tracks(void *x, double v);
void arrangement_track(void *x, double v);
void arrangement_gain1(void *x, double v);
void arrangement_gain2(void *x, double v);
void arrangement_gain(void *x, double v);
void arrangement_offset(void *x, double v);
void arrangement_rt_exporting(void *x, double v);
void arrangement_ready(void *x, double v);
void arrangement_auditioning(void *x, double v);
void arrangement_lead(void *x, double v);
void arrangement_interpolation(void *x, double v);

void arrangement_leads(void *x, double v);
void arrangement_from(void *x, double v);
void arrangement_to(void *x, double v);
void arrangement_go(void *x, double v);
void arrangement_extended_t(void *x, double v);
void arrangement_delayed_go(void *x, double v);
void arrangement_loops(void *x, double v);
void arrangement_zoom1(void *x, double v);
void arrangement_zoom2(void *x, double v);
void arrangement_t_msg(void *x, double v);
void arrangement_debug(void *x, double v);
void arrangement_domain(void *x, double v);
void arrangement_restart(void *x, double v);

typedef struct _arrangement_chan {
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
} t_arrangement_chan;

typedef struct _arrangement {
    t_pxobject t_obj;
    long chans;
    long chans_dsp; // Snapshot used in perform64
    t_arrangement_chan *chan_states;
    t_critical lock;

    long inlet_channels;
    long outlet_channels[7];
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
void arrangement_setvalue(t_arrangement *x, t_symbol *s, long argc, t_atom *argv);
t_max_err arrangement_chans_set(t_arrangement *x, void *attr, long argc, t_atom *argv);

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
    if (!ref) return info;
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
    if (!ref) return;
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

void arrangement_alloc_channels(t_arrangement *x, long n) {
    critical_enter(x->lock);
    if (x->chan_states) {
        for (int i = 0; i < x->chans; i++) {
            if (x->chan_states[i].stem_ref) object_free(x->chan_states[i].stem_ref);
            if (x->chan_states[i].palette_ref) object_free(x->chan_states[i].palette_ref);
            if (x->chan_states[i].product_ref) object_free(x->chan_states[i].product_ref);
            if (x->chan_states[i].comp_ref) object_free(x->chan_states[i].comp_ref);
            if (x->chan_states[i].stats_ref) object_free(x->chan_states[i].stats_ref);
            if (x->chan_states[i].following_ref) object_free(x->chan_states[i].following_ref);
            if (x->chan_states[i].bar_ref) object_free(x->chan_states[i].bar_ref);
        }
        sysmem_freeptr(x->chan_states);
    }
    x->chans = n;
    if (n > 0) {
        x->chan_states = (t_arrangement_chan *)sysmem_newptrclear(n * sizeof(t_arrangement_chan));
        for (int i = 0; i < n; i++) {
            t_arrangement_chan *c = &x->chan_states[i];
            c->stem_ref = buffer_ref_new((t_object *)x, _sym_nothing);
            c->palette_ref = buffer_ref_new((t_object *)x, _sym_nothing);
            c->product_ref = buffer_ref_new((t_object *)x, _sym_nothing);
            c->comp_ref = buffer_ref_new((t_object *)x, _sym_nothing);
            c->stats_ref = buffer_ref_new((t_object *)x, _sym_nothing);
            c->following_ref = buffer_ref_new((t_object *)x, _sym_nothing);
            c->bar_ref = buffer_ref_new((t_object *)x, _sym_nothing);

            c->gain1 = 1; c->gain2 = 1; c->gain = 1; c->lead = 44;
            c->leads = 44100; c->zoom2 = 1;
            crossfade_init(&c->xf_audition, sys_getsr() > 0 ? sys_getsr() : 44100.0, 22.653, 4999.0);
            crossfade_init(&c->xf_interpolation, sys_getsr() > 0 ? sys_getsr() : 44100.0, 22.653, 4999.0);
        }
    } else {
        x->chan_states = NULL;
    }
    critical_exit(x->lock);
}

void ext_main(void *r) {
    common_symbols_init();
    t_class *c = class_new("arrangement~", (method)arrangement_new, (method)arrangement_free, sizeof(t_arrangement), 0L, A_GIMME, 0);

    class_addmethod(c, (method)arrangement_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)arrangement_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)arrangement_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)arrangement_setvalue, "setvalue", A_GIMME, 0);

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

    CLASS_ATTR_LONG(c, "chans", 0, t_arrangement, chans);
    CLASS_ATTR_ACCESSORS(c, "chans", NULL, (method)arrangement_chans_set);
    CLASS_ATTR_DEFAULT(c, "chans", 0, "1");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    arrangement_class = c;
}

void *arrangement_new(t_symbol *s, long argc, t_atom *argv) {
    t_arrangement *x = (t_arrangement *)object_alloc(arrangement_class);
    if (x) {
        critical_new(&x->lock);
        x->chan_states = NULL;
        x->chans = 1;
        x->chans_dsp = 0;
        x->inlet_channels = 0;
        memset(x->outlet_channels, 0, sizeof(x->outlet_channels));

        attr_args_process(x, argc, argv);
        arrangement_alloc_channels(x, x->chans);

        dsp_setup((t_pxobject *)x, 1);
        x->t_obj.z_misc |= Z_MC_INLETS;

        for (int i = 0; i < 7; i++) {
            outlet_new((t_object *)x, "signal");
        }
    }
    return x;
}

void arrangement_free(t_arrangement *x) {
    dsp_free((t_pxobject *)x);
    arrangement_alloc_channels(x, 0);
    critical_free(x->lock);
}

t_max_err arrangement_chans_set(t_arrangement *x, void *attr, long argc, t_atom *argv) {
    if (argc && argv) {
        long n = atom_getlong(argv);
        if (n < 1) n = 1;
        if (n > MC_MAX_CHANS) n = MC_MAX_CHANS;
        if (n != x->chans) {
            arrangement_alloc_channels(x, n);
            // Rebuild DSP chain if active
            if (sys_getdspstate()) {
                object_method(x, gensym("dsp_ask"));
            }
        }
    }
    return MAX_ERR_NONE;
}

void arrangement_assist(t_arrangement *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet %ld: Signal/Messages", a + 1);
    } else {
        switch (a) {
            case 0: sprintf(s, "(MC signal) out1: result"); break;
            case 1: sprintf(s, "(MC signal) out2: t"); break;
            case 2: sprintf(s, "(MC signal) out3: zoom1"); break;
            case 3: sprintf(s, "(MC signal) out4: crossfade mix"); break;
            case 4: sprintf(s, "(MC signal) out5: zoom2"); break;
            case 5: sprintf(s, "(MC signal) out6: t delta < 0"); break;
            case 6: sprintf(s, "(MC signal) out7: domain"); break;
        }
    }
}

t_max_err arrangement_notify(t_arrangement *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        if (x->chan_states) {
            for (int i = 0; i < x->chans; i++) {
                if (x->chan_states[i].stem_ref) buffer_ref_notify(x->chan_states[i].stem_ref, s, msg, sender, data);
                if (x->chan_states[i].palette_ref) buffer_ref_notify(x->chan_states[i].palette_ref, s, msg, sender, data);
                if (x->chan_states[i].product_ref) buffer_ref_notify(x->chan_states[i].product_ref, s, msg, sender, data);
                if (x->chan_states[i].comp_ref) buffer_ref_notify(x->chan_states[i].comp_ref, s, msg, sender, data);
                if (x->chan_states[i].stats_ref) buffer_ref_notify(x->chan_states[i].stats_ref, s, msg, sender, data);
                if (x->chan_states[i].following_ref) buffer_ref_notify(x->chan_states[i].following_ref, s, msg, sender, data);
                if (x->chan_states[i].bar_ref) buffer_ref_notify(x->chan_states[i].bar_ref, s, msg, sender, data);
            }
        }
        critical_exit(x->lock);
    }
    return MAX_ERR_NONE;
}

// Unified message dispatcher
void arrangement_dispatch(t_arrangement *x, long chan_idx, t_symbol *s, long argc, t_atom *argv) {
    if (!x->chan_states || argc < 1) return;

    int start = (chan_idx == 0) ? 0 : (int)chan_idx - 1;
    int end = (chan_idx == 0) ? (int)x->chans : (int)chan_idx;

    if (start < 0 || end > x->chans) return;

    critical_enter(x->lock);
    for (int i = start; i < end; i++) {
        t_arrangement_chan *c = &x->chan_states[i];
        if (s == gensym("stem")) buffer_ref_set(c->stem_ref, atom_getsym(argv));
        else if (s == gensym("palette")) buffer_ref_set(c->palette_ref, atom_getsym(argv));
        else if (s == gensym("product")) buffer_ref_set(c->product_ref, atom_getsym(argv));
        else if (s == gensym("comp")) buffer_ref_set(c->comp_ref, atom_getsym(argv));
        else if (s == gensym("stats")) buffer_ref_set(c->stats_ref, atom_getsym(argv));
        else if (s == gensym("following")) buffer_ref_set(c->following_ref, atom_getsym(argv));
        else if (s == gensym("bar")) buffer_ref_set(c->bar_ref, atom_getsym(argv));

        else if (s == gensym("tracks")) c->tracks = atom_getfloat(argv);
        else if (s == gensym("track")) c->track = atom_getfloat(argv);
        else if (s == gensym("gain1")) c->gain1 = atom_getfloat(argv);
        else if (s == gensym("gain2")) c->gain2 = atom_getfloat(argv);
        else if (s == gensym("gain")) c->gain = atom_getfloat(argv);
        else if (s == gensym("offset")) c->offset = atom_getfloat(argv);
        else if (s == gensym("rt_exporting")) c->rt_exporting = atom_getfloat(argv);
        else if (s == gensym("ready")) c->ready = atom_getfloat(argv);
        else if (s == gensym("auditioning")) c->auditioning = atom_getfloat(argv);
        else if (s == gensym("lead")) c->lead = atom_getfloat(argv);
        else if (s == gensym("interpolation")) c->interpolation = atom_getfloat(argv);

        else if (s == gensym("leads")) c->leads = atom_getfloat(argv);
        else if (s == gensym("from")) c->from = atom_getfloat(argv);
        else if (s == gensym("to")) c->to = atom_getfloat(argv);
        else if (s == gensym("go")) c->go = atom_getfloat(argv);
        else if (s == gensym("extended_t")) c->extended_t = atom_getfloat(argv);
        else if (s == gensym("delayed_go")) c->delayed_go = atom_getfloat(argv);
        else if (s == gensym("loops")) c->loops = atom_getfloat(argv);
        else if (s == gensym("zoom1")) c->zoom1 = atom_getfloat(argv);
        else if (s == gensym("zoom2")) c->zoom2 = atom_getfloat(argv);
        else if (s == gensym("t")) c->t = atom_getfloat(argv);
        else if (s == gensym("debug")) c->debug = atom_getfloat(argv);
        else if (s == gensym("domain")) c->domain = atom_getfloat(argv);
        else if (s == gensym("restart")) c->restart = atom_getfloat(argv);
    }
    critical_exit(x->lock);
}

void arrangement_setvalue(t_arrangement *x, t_symbol *s, long argc, t_atom *argv) {
    if (argc >= 2) {
        long chan = atom_getlong(argv);
        t_symbol *sel = atom_getsym(argv + 1);
        arrangement_dispatch(x, chan, sel, argc - 2, argv + 2);
    }
}

// Map standard messages to dispatcher
void arrangement_stem(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("stem"), 1, (t_atom *)&s); }
void arrangement_palette(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("palette"), 1, (t_atom *)&s); }
void arrangement_product(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("product"), 1, (t_atom *)&s); }
void arrangement_comp(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("comp"), 1, (t_atom *)&s); }
void arrangement_stats(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("stats"), 1, (t_atom *)&s); }
void arrangement_following(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("following"), 1, (t_atom *)&s); }
void arrangement_bar(void *x, t_symbol *s) { arrangement_dispatch((t_arrangement *)x, 0, gensym("bar"), 1, (t_atom *)&s); }

#define AR_PARAM_METHOD(name, sel) void arrangement_##name(void *x, double v) { t_atom a; atom_setfloat(&a, (float)v); arrangement_dispatch((t_arrangement *)x, 0, gensym(sel), 1, &a); }
AR_PARAM_METHOD(tracks, "tracks") AR_PARAM_METHOD(track, "track") AR_PARAM_METHOD(gain1, "gain1") AR_PARAM_METHOD(gain2, "gain2") AR_PARAM_METHOD(gain, "gain")
AR_PARAM_METHOD(offset, "offset") AR_PARAM_METHOD(rt_exporting, "rt_exporting") AR_PARAM_METHOD(ready, "ready") AR_PARAM_METHOD(auditioning, "auditioning")
AR_PARAM_METHOD(lead, "lead") AR_PARAM_METHOD(interpolation, "interpolation") AR_PARAM_METHOD(leads, "leads") AR_PARAM_METHOD(from, "from") AR_PARAM_METHOD(to, "to")
AR_PARAM_METHOD(go, "go") AR_PARAM_METHOD(extended_t, "extended_t") AR_PARAM_METHOD(delayed_go, "delayed_go") AR_PARAM_METHOD(loops, "loops")
AR_PARAM_METHOD(zoom1, "zoom1") AR_PARAM_METHOD(zoom2, "zoom2") AR_PARAM_METHOD(t_msg, "t") AR_PARAM_METHOD(debug, "debug") AR_PARAM_METHOD(domain, "domain")
AR_PARAM_METHOD(restart, "restart")

void arrangement_dsp64(t_arrangement *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    x->inlet_channels = (long)(uintptr_t)object_method(dsp64, gensym("getnumchannels"), (t_object *)x, 0);
    x->chans_dsp = x->chans;
    for (int i = 0; i < 7; i++) {
        object_method(dsp64, gensym("setnumchannels"), (t_object *)x, i, x->chans_dsp);
        x->outlet_channels[i] = x->chans_dsp;
    }

    if (x->chan_states) {
        for (int i = 0; i < x->chans_dsp; i++) {
            crossfade_update_params(&x->chan_states[i].xf_audition, samplerate, 22.653, 4999.0);
            crossfade_update_params(&x->chan_states[i].xf_interpolation, samplerate, 22.653, 4999.0);
        }
    }

    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)arrangement_perform64, 0, NULL);
}

void arrangement_perform64(t_arrangement *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double samplerate = sys_getsr();
    if (samplerate <= 0) samplerate = 44100.0;

    // Zero all outputs first
    for (int i = 0; i < numouts; i++) {
        if (outs[i]) memset(outs[i], 0, sampleframes * sizeof(double));
    }

    if (critical_tryenter(x->lock) != MAX_ERR_NONE) return;

    if (!x->chan_states) {
        critical_exit(x->lock);
        return;
    }

    long active_chans = x->chans_dsp;
    if (active_chans > x->chans) active_chans = x->chans;

    for (int c = 0; c < active_chans; c++) {
        t_arrangement_chan *chan = &x->chan_states[c];

        double *in_ramp = (x->inlet_channels > 0) ? ins[c % x->inlet_channels] : NULL;

        double *out_vecs[7];
        for (int j = 0; j < 7; j++) {
            long out_idx = j * x->chans_dsp + c;
            out_vecs[j] = (out_idx < numouts) ? outs[out_idx] : NULL;
        }

        t_arrangement_buffer_info stem_info = lock_buffer(chan->stem_ref);
        t_arrangement_buffer_info palette_info = lock_buffer(chan->palette_ref);
        t_arrangement_buffer_info stats_info = lock_buffer(chan->stats_ref);
        t_arrangement_buffer_info bar_info = lock_buffer(chan->bar_ref);
        t_arrangement_buffer_info comp_info = lock_buffer(chan->comp_ref);

        for (int i = 0; i < sampleframes; i++) {
            double o[7] = {0, 0, 0, 0, 0, 0, 0};
            double elapsed = in_ramp ? in_ramp[i] : (double)chan->elapsed_counter;

            if (chan->ready || chan->rt_exporting) {
                if (chan->domain == 0.0) {
                    chan->domain = round(mstosamps(peek_info(stats_info, 1), samplerate));
                }
                int follow = (chan->to - chan->from == chan->domain);
                if (follow) {
                    chan->domain = round(mstosamps(peek_info(stats_info, 1), samplerate));
                    chan->to = chan->domain;
                    chan->zoom1 = fmax(0.0, chan->t - fmax(chan->lead, 88200.0));
                    chan->zoom2 = fmin(chan->domain, chan->t + fmax(chan->lead, 88200.0)) - chan->zoom1;
                }
                poke_info(stats_info, 10, chan->domain > 0 ? 1.0f : 0.0f);

                double bar_length = mstosamps(peek_info(bar_info, 0), samplerate);
                double start = fmax(0.0, gen_round(chan->from - chan->lead, bar_length, "floor"));
                double end = fmin(chan->domain, gen_round(chan->to + chan->lead, bar_length, "ceil"));

                chan->t = elapsed - chan->go + start;
                chan->extended_t = elapsed - chan->delayed_go + start;

                int from_changed = (chan->from != chan->last_from);
                int to_changed = (chan->to != chan->last_to);

                if (((from_changed || to_changed) && chan->from != 0) || chan->t > end || chan->restart) {
                    chan->restart = 0;
                    chan->go = elapsed;
                    chan->loops += 1.0;
                    chan->zoom1 = fmax(0.0, start);
                    chan->zoom2 = fmin(chan->domain, end) - chan->zoom1;
                    if (chan->from == chan->to) {
                        chan->go -= chan->from;
                        chan->from = 0;
                        chan->to = chan->domain;
                    }
                }
                chan->last_from = chan->from;
                chan->last_to = chan->to;

                if (chan->track == (double)(c + 1) && chan->auditioning) {
                    int on = (chan->t > chan->from && chan->t < chan->to);
                    double surround = peek_info(stem_info, chan->t);
                    double diff_to_from = chan->to - chan->from;
                    double m = (diff_to_from != 0) ? (chan->gain2 - chan->gain1) / diff_to_from : 0;
                    double comp_gain = (chan->t - chan->from) * m + chan->gain1;
                    double audition = peek_info(palette_info, chan->t + chan->offset) * comp_gain;

                    double mix1, mix2, sum;
                    int busy;
                    crossfade_process(&chan->xf_audition, (double)on, surround, audition, &mix1, &mix2, &sum, &busy);
                    o[3] = mix2;
                    o[0] = sum;

                    if (!on && ((double)busy - chan->last_out6_busy < 0)) {
                        poke_info(comp_info, chan->t, 1.0f);
                        if (comp_info.samples) {
                            t_buffer_obj *comp_obj = buffer_ref_getobject(chan->comp_ref);
                            if (comp_obj) buffer_setdirty(comp_obj);
                        }
                    }
                    chan->last_out6_busy = (double)busy;
                } else {
                    o[0] = peek_info(stem_info, chan->t);
                }

                if (chan->interpolation) {
                    double trail = peek_info(stem_info, chan->extended_t) * chan->gain2;
                    double mix1, mix2, sum;
                    int busy;
                    crossfade_process(&chan->xf_interpolation, chan->loops, trail, o[0], &mix1, &mix2, &sum, &busy);
                    o[0] = sum;
                    if ((double)busy - chan->last_fading_busy < 0) {
                        chan->delayed_go = chan->go;
                    }
                    chan->last_fading_busy = (double)busy;
                }
            }

            if (chan->domain <= 0) chan->t *= 0;
            o[0] *= chan->gain;
            o[1] = chan->t;
            o[2] = chan->zoom1;
            o[4] = chan->zoom2;
            o[5] = (chan->t - chan->last_t < 0) ? 1.0 : 0.0;
            o[6] = chan->domain;

            chan->last_t = chan->t;

            for (int j = 0; j < 7; j++) {
                if (out_vecs[j]) out_vecs[j][i] = o[j];
            }

            chan->elapsed_counter++;
        }

        unlock_buffer(chan->stem_ref);
        unlock_buffer(chan->palette_ref);
        unlock_buffer(chan->stats_ref);
        unlock_buffer(chan->bar_ref);
        unlock_buffer(chan->comp_ref);
    }
    critical_exit(x->lock);
}

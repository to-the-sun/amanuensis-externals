#include "ext.h"
#include "ext_obex.h"
#include "ext_hashtab.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "z_dsp.h"
#include "../shared/visualize.h"
#include <string.h>
#include <math.h>

typedef struct _threads {
    t_object t_obj;
    t_symbol *poly_prefix;
    t_hashtab *palette_map;
    void *proxy;
    long inlet_num;
    long verbose;
    void *verbose_log_outlet;
    t_hashtab *buffer_refs;
} t_threads;

void *threads_new(t_symbol *s, long argc, t_atom *argv);
void threads_free(t_threads *x);
void threads_list(t_threads *x, t_symbol *s, long argc, t_atom *argv);
void threads_anything(t_threads *x, t_symbol *s, long argc, t_atom *argv);
t_max_err threads_notify(t_threads *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void threads_assist(t_threads *x, void *b, long m, long a, char *s);
void threads_verbose_log(t_threads *x, const char *fmt, ...);
void threads_process_data(t_threads *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms);

static t_class *threads_class;

void threads_verbose_log(t_threads *x, const char *fmt, ...) {
    if (x->verbose && x->verbose_log_outlet) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        outlet_anything(x->verbose_log_outlet, gensym(buf), 0, NULL);
    }
}

void ext_main(void *r) {
    t_class *c = class_new("threads~", (method)threads_new, (method)threads_free, sizeof(t_threads), 0L, A_GIMME, 0);

    class_addmethod(c, (method)threads_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)threads_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)threads_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)threads_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_threads, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");

    class_register(CLASS_BOX, c);
    threads_class = c;

    common_symbols_init();
}

void *threads_new(t_symbol *s, long argc, t_atom *argv) {
    t_threads *x = (t_threads *)object_alloc(threads_class);

    if (x) {
        x->poly_prefix = _sym_nothing;
        x->verbose = 0;

        // Unconditionally create the outlet
        x->verbose_log_outlet = outlet_new((t_object *)x, NULL);

        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->poly_prefix = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        if (x->poly_prefix == _sym_nothing) {
            object_error((t_object *)x, "missing polybuffer~ prefix argument");
        }

        // Unconditionally initialize visualization socket
        if (visualize_init() != 0) {
            object_error((t_object *)x, "failed to initialize visualization socket");
        }

        x->palette_map = hashtab_new(0);
        hashtab_flags(x->palette_map, OBJ_FLAG_REF);

        x->buffer_refs = hashtab_new(0);

        x->proxy = proxy_new(x, 1, &x->inlet_num);
    }
    return x;
}

void threads_free(t_threads *x) {
    object_free(x->proxy);
    if (x->palette_map) {
        hashtab_clear(x->palette_map);
        object_free(x->palette_map);
    }
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
    visualize_cleanup();
}

t_max_err threads_notify(t_threads *x, t_symbol *s, t_symbol *msg, void *sender, void *data) {
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
    return MAX_ERR_NONE;
}

void threads_assist(t_threads *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(list) [palette, track, bar, offset] Data from crucible, (symbol) clear"); break;
            case 1: sprintf(s, "(list) palette-index pairs, (symbol) clear"); break;
        }
    } else { // ASSIST_OUTLET
        sprintf(s, "Verbose Logging Outlet");
    }
}

void threads_process_data(t_threads *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms) {
    // 1. Palette mapping lookup
    t_atom_long chan_index = -1;
    hashtab_lookuplong(x->palette_map, palette, &chan_index);

    if (chan_index == -1 && offset_ms != 0.0) {
        threads_verbose_log(x, "WRITE SKIPPED: Palette '%s' not mapped to any channel", palette->s_name);
        if (offset_ms == -999999.0) {
             char json[256];
             // Send with num_chans = -1 to indicate unmapped/reach event
             snprintf(json, 256, "{\"track\": %lld, \"ms\": %.2f, \"chan\": -1, \"val\": %.2f, \"num_chans\": -1}",
                      (long long)track, bar_ms, offset_ms);
             visualize(json);
        }
        return;
    }

    // 2. Buffer lookup
    char bufname[256];
    snprintf(bufname, 256, "%s.%lld", x->poly_prefix->s_name, (long long)track);
    t_symbol *s_bufname = gensym(bufname);

    t_buffer_ref *buf_ref = NULL;
    hashtab_lookup(x->buffer_refs, s_bufname, (t_object **)&buf_ref);
    if (!buf_ref) {
        buf_ref = buffer_ref_new((t_object *)x, s_bufname);
        hashtab_store(x->buffer_refs, s_bufname, (t_object *)buf_ref);
    }
    t_buffer_obj *b = buffer_ref_getobject(buf_ref);

    if (!b) {
        threads_verbose_log(x, "Error: buffer %s not found", s_bufname->s_name);
        return;
    }

    double sr = buffer_getsamplerate(b);
    if (sr <= 0) sr = sys_getsr();
    if (sr <= 0) sr = 44100.0; // fallback

    // 3. Sample index conversion
    long sample_index = (long)round(bar_ms * sr / 1000.0);

    // 4. Offset value conversion
    double write_val;
    if (offset_ms == 0.0) {
        write_val = 0.0;
    } else if (offset_ms == -999999.0) {
        write_val = -999999.0;
    } else {
        write_val = round(offset_ms * sr / 1000.0);
    }

    // 5. Writing to buffer

    // Visualization: Send single packet regardless of buffer bounds (offload work to script)
    char json[256];
    // We'll get num_chans here just for visualization, then again inside critical if needed
    long num_chans_viz = buffer_getchannelcount(b);
    snprintf(json, 256, "{\"track\": %lld, \"ms\": %.2f, \"chan\": %lld, \"val\": %.2f, \"num_chans\": %ld}",
             (long long)track, bar_ms, (long long)chan_index, offset_ms, num_chans_viz);
    visualize(json);

    critical_enter(0);
    long num_frames = buffer_getframecount(b);
    long num_chans = buffer_getchannelcount(b);

    if (sample_index >= 0 && sample_index < num_frames) {
        float *samples = buffer_locksamples(b);
        if (samples) {
            if (offset_ms == 0.0) {
                // Write 0 to all channels
                for (long c = 0; c < num_chans; c++) {
                    samples[sample_index * num_chans + c] = 0.0f;
                }
            } else {
                // Write write_val to chan_index, -999999 to others
                for (long c = 0; c < num_chans; c++) {
                    if (c == (long)chan_index) {
                        samples[sample_index * num_chans + c] = (float)write_val;
                    } else {
                        samples[sample_index * num_chans + c] = -999999.0f;
                    }
                }
            }
            buffer_unlocksamples(b);
            buffer_setdirty(b);
            critical_exit(0);
            threads_verbose_log(x, "WRITE SUCCESS: Track %lld, Sample %ld, Channel %lld, Value %.2f (Palette: %s)", (long long)track, sample_index, (long long)chan_index, offset_ms, palette->s_name);
        } else {
            critical_exit(0);
            threads_verbose_log(x, "WRITE FAILED: Could not lock samples for buffer %s", s_bufname->s_name);
        }
    } else {
        critical_exit(0);
        threads_verbose_log(x, "WRITE SKIPPED: Sample index %ld out of bounds for buffer %s", sample_index, s_bufname->s_name);
    }
}

void threads_list(t_threads *x, t_symbol *s, long argc, t_atom *argv) {
    if (proxy_getinlet((t_object *)x) != 0) return;

    if (argc < 4) {
        object_error((t_object *)x, "threads~ expects a list of 4 items: [palette:sym, track:int, bar:int, offset:float]");
        return;
    }

    if (atom_gettype(argv) != A_SYM) {
        object_error((t_object *)x, "threads~ first item in list must be a palette name (symbol)");
        return;
    }
    t_symbol *palette = atom_getsym(argv);
    t_atom_long track = atom_getlong(argv + 1);
    double bar_ms = atom_getfloat(argv + 2);
    double offset_ms = atom_getfloat(argv + 3);

    threads_process_data(x, palette, track, bar_ms, offset_ms);
}

void threads_anything(t_threads *x, t_symbol *s, long argc, t_atom *argv) {
    long inlet = proxy_getinlet((t_object *)x);

    if (inlet == 1) {
        if (s == gensym("clear")) {
            hashtab_clear(x->palette_map);
        } else if (argc >= 1 && (atom_gettype(argv) == A_LONG || atom_gettype(argv) == A_FLOAT)) {
            // Store palette (s) -> index (argv[0])
            t_atom_long index = atom_getlong(argv);
            hashtab_storelong(x->palette_map, s, index);
        }
    } else if (inlet == 0) {
        if (s == gensym("clear") && argc == 0) {
            // Note: This operation is synchronous and blocks the message thread
            // until all buffers in the polybuffer~ have been cleared.
            object_post((t_object *)x, "threads~: received clear message on inlet 0");
            threads_verbose_log(x, "CLEARING START: Prefix '%s' on inlet 0", x->poly_prefix->s_name);
            char bufname[256];
            int i = 1;
            int cleared_count = 0;
            t_buffer_ref *temp_ref = NULL;

            while (1) {
                snprintf(bufname, 256, "%s.%d", x->poly_prefix->s_name, i);
                t_symbol *s_member = gensym(bufname);

                if (temp_ref == NULL) {
                    temp_ref = buffer_ref_new((t_object *)x, s_member);
                } else {
                    buffer_ref_set(temp_ref, s_member);
                }

                t_buffer_obj *b = buffer_ref_getobject(temp_ref);
                if (!b) break;

                critical_enter(0);
                float *samples = buffer_locksamples(b);
                if (samples) {
                    long num_chans = buffer_getchannelcount(b);
                    long num_frames = buffer_getframecount(b);
                    memset(samples, 0, num_frames * num_chans * sizeof(float));
                    buffer_unlocksamples(b);
                    buffer_setdirty(b);
                    critical_exit(0);
                    threads_verbose_log(x, "CLEARED BUFFER: %s", bufname);
                    cleared_count++;
                } else {
                    critical_exit(0);
                    threads_verbose_log(x, "CLEARING FAILED: Could not lock samples for buffer %s", bufname);
                }
                i++;
            }
            if (temp_ref) object_free(temp_ref);

            object_post((t_object *)x, "threads~: cleared %d buffers", cleared_count);
            threads_verbose_log(x, "CLEARING END: Total %d buffers cleared", cleared_count);

            visualize("{\"clear\": 1}");
            object_post((t_object *)x, "threads~: sent clear command to visualizer");
        } else if (argc >= 3) {
            // Handle anything on inlet 0 (like the "-" reach message)
            t_symbol *palette = s;
            t_atom_long track = atom_getlong(argv);
            double bar_ms = atom_getfloat(argv + 1);
            double offset_ms = atom_getfloat(argv + 2);
            threads_process_data(x, palette, track, bar_ms, offset_ms);
        }
    }
}

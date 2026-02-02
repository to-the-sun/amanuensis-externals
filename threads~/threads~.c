#include "ext.h"
#include "ext_obex.h"
#include "ext_hashtab.h"
#include "ext_buffer.h"
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
} t_threads;

void *threads_new(t_symbol *s, long argc, t_atom *argv);
void threads_free(t_threads *x);
void threads_list(t_threads *x, t_symbol *s, long argc, t_atom *argv);
void threads_anything(t_threads *x, t_symbol *s, long argc, t_atom *argv);
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
    visualize_cleanup();
}

void threads_assist(t_threads *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(list) [palette, track, bar, offset] Data from crucible"); break;
            case 1: sprintf(s, "(list) palette-index pairs, (symbol) clear"); break;
        }
    } else { // ASSIST_OUTLET
        sprintf(s, "Verbose Logging Outlet");
    }
}

void threads_process_data(t_threads *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms) {
    threads_verbose_log(x, "Processing: palette=%s track=%lld bar_ms=%.2f offset_ms=%.2f",
                        palette->s_name, (long long)track, bar_ms, offset_ms);

    // 1. Palette mapping lookup
    t_atom_long chan_index = -1;
    hashtab_lookuplong(x->palette_map, palette, &chan_index);

    // If palette not found and it's not a clear operation (0.0), skip writing to buffer
    // But we might still want to visualize reach messages (offset -999999.0) if we knew the channel.
    // However, if we don't know the channel, we can't write to the buffer.
    if (chan_index == -1 && offset_ms != 0.0) {
        threads_verbose_log(x, "Palette '%s' not mapped to any channel. Skipping buffer write.", palette->s_name);
        // Special case: if it's a reach message, we might still want to visualize it for track activity.
        // We'll send it to visualizer on channel -1 (script will handle it as a global track tick or ignore)
        if (offset_ms == -999999.0) {
             char json[256];
             snprintf(json, 256, "{\"track\": %lld, \"channel\": -1, \"ms\": %.2f, \"val\": %.2f}", (long long)track, bar_ms, offset_ms);
             visualize(json);
        }
        return;
    }

    // 2. Buffer lookup
    char bufname[256];
    snprintf(bufname, 256, "%s.%lld", x->poly_prefix->s_name, (long long)track);
    t_symbol *s_bufname = gensym(bufname);

    t_buffer_ref *buf_ref = buffer_ref_new((t_object *)x, s_bufname);
    t_buffer_obj *b = buffer_ref_getobject(buf_ref);

    if (!b) {
        threads_verbose_log(x, "Error: buffer %s not found", s_bufname->s_name);
        object_free(buf_ref);
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
    long num_chans = buffer_getchannelcount(b);
    long num_frames = buffer_getframecount(b);

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

            // Visualization
            for (long c = 0; c < num_chans; c++) {
                double val_to_send;
                if (offset_ms == 0.0) {
                    val_to_send = 0.0;
                } else {
                    val_to_send = (c == (long)chan_index) ? offset_ms : -999999.0;
                }
                char json[256];
                snprintf(json, 256, "{\"track\": %lld, \"channel\": %ld, \"ms\": %.2f, \"val\": %.2f}", (long long)track, c, bar_ms, val_to_send);
                visualize(json);
            }
        }
    }

    object_free(buf_ref);
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
        // Handle anything on inlet 0 (like the "-" reach message)
        if (argc >= 3) {
            t_symbol *palette = s;
            t_atom_long track = atom_getlong(argv);
            double bar_ms = atom_getfloat(argv + 1);
            double offset_ms = atom_getfloat(argv + 2);
            threads_process_data(x, palette, track, bar_ms, offset_ms);
        }
    }
}

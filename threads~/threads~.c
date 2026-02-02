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
} t_threads;

void *threads_new(t_symbol *s, long argc, t_atom *argv);
void threads_free(t_threads *x);
void threads_list(t_threads *x, t_symbol *s, long argc, t_atom *argv);
void threads_anything(t_threads *x, t_symbol *s, long argc, t_atom *argv);
void threads_assist(t_threads *x, void *b, long m, long a, char *s);

static t_class *threads_class;

void ext_main(void *r) {
    t_class *c = class_new("threads~", (method)threads_new, (method)threads_free, sizeof(t_threads), 0L, A_GIMME, 0);

    class_addmethod(c, (method)threads_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)threads_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)threads_assist, "assist", A_CANT, 0);

    class_register(CLASS_BOX, c);
    threads_class = c;

    common_symbols_init();
    visualize_init();
}

void *threads_new(t_symbol *s, long argc, t_atom *argv) {
    t_threads *x = (t_threads *)object_alloc(threads_class);

    if (x) {
        x->poly_prefix = _sym_nothing;
        if (argc > 0 && atom_gettype(argv) == A_SYM) {
            x->poly_prefix = atom_getsym(argv);
        } else {
            object_error((t_object *)x, "missing polybuffer~ prefix argument");
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
}

void threads_assist(t_threads *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(list) [palette, track, bar, offset] Data from crucible"); break;
            case 1: sprintf(s, "(list) palette-index pairs, (symbol) clear"); break;
        }
    }
}

// Placeholder for other methods to be implemented in next steps
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

    // 1. Buffer lookup
    char bufname[256];
    snprintf(bufname, 256, "%s.%ld", x->poly_prefix->s_name, track);
    t_symbol *s_bufname = gensym(bufname);

    t_buffer_ref *buf_ref = buffer_ref_new((t_object *)x, s_bufname);
    t_buffer_obj *b = buffer_ref_getobject(buf_ref);

    if (!b) {
        object_free(buf_ref);
        return;
    }

    double sr = buffer_getsamplerate(b);
    if (sr <= 0) sr = sys_getsr();
    if (sr <= 0) sr = 44100.0; // fallback

    // 2. Sample index conversion (Bar)
    long sample_index = (long)round(bar_ms * sr / 1000.0);

    // 3. Offset value conversion
    double write_val;
    if (offset_ms == 0.0) {
        write_val = 0.0;
    } else if (offset_ms == -999999.0) {
        write_val = -999999.0;
    } else {
        write_val = round(offset_ms * sr / 1000.0);
    }

    // 4. Channel lookup
    t_atom_long chan_index = -1;
    hashtab_lookuplong(x->palette_map, palette, &chan_index);

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
                    if (c == chan_index) {
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
                    val_to_send = (c == chan_index) ? offset_ms : -999999.0;
                }
                char json[256];
                snprintf(json, 256, "{\"track\": %lld, \"channel\": %lld, \"ms\": %.2f, \"val\": %.2f}", (long long)track, (long long)c, bar_ms, val_to_send);
                visualize(json);
            }
        }
    }

    object_free(buf_ref);
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
    }
}

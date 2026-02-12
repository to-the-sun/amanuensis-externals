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
#include "../shared/visualize.h"
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

typedef struct _threads {
    t_pxobject t_obj;
    t_symbol *poly_prefix;
    t_hashtab *palette_map;
    void *proxy_rescript;
    void *proxy_palette;
    long inlet_num;
    long verbose;
    void *verbose_log_outlet;
    void *signal_outlet;
    t_hashtab *buffer_refs;
    t_buffer_ref *bar_buffer_ref;
    t_buffer_ref *track1_ref;

    t_symbol *audio_dict_name;
    double last_ramp_val;
    double last_scan_val;
    t_fifo_entry hit_bars[4096];
    int fifo_head;
    int fifo_tail;
    t_qelem *audio_qelem;
    t_critical lock;
    long max_track_seen;
    long max_tracks;
} t_threads;

void *threads_new(t_symbol *s, long argc, t_atom *argv);
void threads_free(t_threads *x);
void threads_list(t_threads *x, t_symbol *s, long argc, t_atom *argv);
void threads_anything(t_threads *x, t_symbol *s, long argc, t_atom *argv);
t_max_err threads_notify(t_threads *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void threads_assist(t_threads *x, void *b, long m, long a, char *s);
void threads_verbose_log(t_threads *x, const char *fmt, ...);
void threads_process_data(t_threads *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms);
void threads_rescript(t_threads *x, t_symbol *dict_name);
double threads_get_bar_length(t_threads *x);
void threads_dsp64(t_threads *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void threads_perform64(t_threads *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void threads_audio_qtask(t_threads *x);

static t_class *threads_class;

void threads_verbose_log(t_threads *x, const char *fmt, ...) {
    if (x->verbose && x->verbose_log_outlet) {
        char buf[1024];
        char final_buf[1100];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);
        snprintf(final_buf, 1100, "threads~: %s", buf);
        outlet_anything(x->verbose_log_outlet, gensym(final_buf), 0, NULL);
    }
}

void ext_main(void *r) {
    t_class *c = class_new("threads~", (method)threads_new, (method)threads_free, sizeof(t_threads), 0L, A_GIMME, 0);

    class_addmethod(c, (method)threads_list, "list", A_GIMME, 0);
    class_addmethod(c, (method)threads_anything, "anything", A_GIMME, 0);
    class_addmethod(c, (method)threads_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)threads_notify, "notify", A_CANT, 0);
    class_addmethod(c, (method)threads_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "verbose", 0, t_threads, verbose);
    CLASS_ATTR_STYLE_LABEL(c, "verbose", 0, "onoff", "Enable Verbose Logging");

    CLASS_ATTR_LONG(c, "tracks", 0, t_threads, max_tracks);
    CLASS_ATTR_LABEL(c, "tracks", 0, "Number of Tracks");

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    threads_class = c;

    common_symbols_init();
}

void *threads_new(t_symbol *s, long argc, t_atom *argv) {
    t_threads *x = (t_threads *)object_alloc(threads_class);

    if (x) {
        x->poly_prefix = _sym_nothing;
        x->verbose = 0;
        x->audio_dict_name = _sym_nothing;
        x->last_ramp_val = -1.0;
        x->last_scan_val = -1.0;
        x->fifo_head = 0;
        x->fifo_tail = 0;

        // Create outlets from right to left
        x->verbose_log_outlet = outlet_new((t_object *)x, NULL);
        x->signal_outlet = outlet_new((t_object *)x, "signal");

        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->poly_prefix = atom_getsym(argv);
            argc--;
            argv++;
        }

        x->max_tracks = 4; // Default
        if (argc > 0 && (atom_gettype(argv) == A_LONG || atom_gettype(argv) == A_FLOAT)) {
            x->max_tracks = atom_getlong(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        if (x->poly_prefix == _sym_nothing) {
            object_error((t_object *)x, "missing polybuffer~ prefix argument");
        } else {
            char t1name[256];
            snprintf(t1name, 256, "%s.1", x->poly_prefix->s_name);
            x->track1_ref = buffer_ref_new((t_object *)x, gensym(t1name));
        }

        // Unconditionally initialize visualization socket
        if (visualize_init() != 0) {
            object_error((t_object *)x, "failed to initialize visualization socket");
        }

        x->palette_map = hashtab_new(0);
        hashtab_flags(x->palette_map, OBJ_FLAG_REF);

        x->buffer_refs = hashtab_new(0);
        x->bar_buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        x->proxy_palette = proxy_new(x, 2, &x->inlet_num);
        x->proxy_rescript = proxy_new(x, 1, &x->inlet_num);

        critical_new(&x->lock);
        x->max_track_seen = 0;

        if (x->verbose) {
            object_post((t_object *)x, "threads~: Initialized with %ld tracks", x->max_tracks);
        }
        threads_verbose_log(x, "Initialized with %ld tracks", x->max_tracks);

        dsp_setup((t_pxobject *)x, 1);
        x->audio_qelem = qelem_new(x, (method)threads_audio_qtask);
    }
    return x;
}

void threads_free(t_threads *x) {
    dsp_free((t_pxobject *)x);
    critical_free(x->lock);
    if (x->audio_qelem) qelem_free(x->audio_qelem);

    object_free(x->proxy_rescript);
    object_free(x->proxy_palette);
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
    if (x->bar_buffer_ref) {
        object_free(x->bar_buffer_ref);
    }
    if (x->track1_ref) {
        object_free(x->track1_ref);
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
    if (x->bar_buffer_ref) {
        buffer_ref_notify(x->bar_buffer_ref, s, msg, sender, data);
    }
    if (x->track1_ref) {
        buffer_ref_notify(x->track1_ref, s, msg, sender, data);
    }
    return MAX_ERR_NONE;
}

void threads_assist(t_threads *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "Inlet 1 (signal/list): Data from crucible, (symbol) clear"); break;
            case 1: sprintf(s, "Inlet 2 (symbol): Dictionary name for rescript/reference"); break;
            case 2: sprintf(s, "Inlet 3 (list): palette-index pairs, (symbol) clear"); break;
        }
    } else { // ASSIST_OUTLET
        switch (a) {
            case 0: sprintf(s, "Outlet 1 (signal): Scan Head Position (ms)"); break;
            case 1: sprintf(s, "Outlet 2 (anything): Verbose Logging Outlet"); break;
        }
    }
}

double threads_get_bar_length(t_threads *x) {
    double bar_length = 0;
    t_buffer_obj *b = buffer_ref_getobject(x->bar_buffer_ref);
    if (b) {
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


void threads_rescript(t_threads *x, t_symbol *dict_name) {
    double bar_length = threads_get_bar_length(x);

    if (bar_length <= 0) {
        object_error((t_object *)x, "threads~: bar buffer~ not found or empty, cannot perform rescript");
        return;
    }

    t_dictionary *dict = dictobj_findregistered_retain(dict_name);
    if (!dict) {
        threads_verbose_log(x, "CACHING FAILED: dictionary '%s' not found", dict_name->s_name);
        object_error((t_object *)x, "threads~: could not find dictionary named %s", dict_name->s_name);
        return;
    }

    for (int track_val = 1; track_val <= x->max_tracks; track_val++) {
        char tstr[64];
        snprintf(tstr, 64, "%d", track_val);
        t_symbol *track_sym = gensym(tstr);
        t_dictionary *track_dict = NULL;

        if (dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) {
            continue; // Support sparse tracks
        }

        long num_bars = 0;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        for (long j = 0; j < num_bars; j++) {
            t_symbol *bar_key = bar_keys[j];
            double bar_ms = atof(bar_key->s_name);
            t_dictionary *bar_dict = NULL;
            dictionary_getdictionary(track_dict, bar_key, (t_object **)&bar_dict);
            if (!bar_dict) continue;

            t_symbol *palette = _sym_nothing;
            double offset = 0.0;

            t_atomarray *palette_aa = NULL;
            t_atom palette_atom;
            if (dictionary_getatomarray(bar_dict, gensym("palette"), (t_object **)&palette_aa) == MAX_ERR_NONE && palette_aa) {
                t_atom p_atom;
                if (atomarray_getindex(palette_aa, 0, &p_atom) == MAX_ERR_NONE) palette = atom_getsym(&p_atom);
            } else if (dictionary_getatom(bar_dict, gensym("palette"), &palette_atom) == MAX_ERR_NONE) {
                palette = atom_getsym(&palette_atom);
            }

            t_atomarray *offset_aa = NULL;
            t_atom offset_atom;
            if (dictionary_getatomarray(bar_dict, gensym("offset"), (t_object **)&offset_aa) == MAX_ERR_NONE && offset_aa) {
                t_atom o_atom;
                if (atomarray_getindex(offset_aa, 0, &o_atom) == MAX_ERR_NONE) offset = atom_getfloat(&o_atom);
            } else if (dictionary_getatom(bar_dict, gensym("offset"), &offset_atom) == MAX_ERR_NONE) {
                offset = atom_getfloat(&offset_atom);
            }

            threads_process_data(x, palette, (t_atom_long)track_val, bar_ms, offset);

            double next_bar_ms = bar_ms + bar_length;
            char next_bar_str[64];
            snprintf(next_bar_str, 64, "%ld", (long)next_bar_ms);
            t_symbol *next_bar_sym = gensym(next_bar_str);

            if (!dictionary_hasentry(track_dict, next_bar_sym)) {
                 threads_process_data(x, gensym("-"), (t_atom_long)track_val, next_bar_ms, -999999.0);
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }
    dictobj_release(dict);
}

void threads_process_data(t_threads *x, t_symbol *palette, t_atom_long track, double bar_ms, double offset_ms) {
    // 1. Palette mapping lookup
    t_atom_long chan_index = -1;
    t_max_err err = hashtab_lookuplong(x->palette_map, palette, &chan_index);

    if (err != MAX_ERR_NONE && offset_ms != 0.0 && palette != gensym("-")) {
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

    if (track > x->max_track_seen) x->max_track_seen = track;

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
        float *samples = NULL;
        int retries = 0;
        for (retries = 0; retries < 10; retries++) {
            samples = buffer_locksamples(b);
            if (samples) break;
            systhread_sleep(1);
        }

        if (samples) {
            if (offset_ms == 0.0 || chan_index < 0) {
                // Write write_val (which is 0 if offset_ms is 0) to all channels
                for (long c = 0; c < num_chans; c++) {
                    samples[sample_index * num_chans + c] = (float)write_val;
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
            if (retries > 0) {
                threads_verbose_log(x, "WRITE SUCCESS: Track %lld, Position %.2f ms, Channel %lld, Offset %.2f ms (Palette: %s) [Retries: %d]", (long long)track, bar_ms, (long long)chan_index, offset_ms, palette->s_name, retries);
            } else {
                threads_verbose_log(x, "WRITE SUCCESS: Track %lld, Position %.2f ms, Channel %lld, Offset %.2f ms (Palette: %s)", (long long)track, bar_ms, (long long)chan_index, offset_ms, palette->s_name);
            }
        } else {
            critical_exit(0);
            threads_verbose_log(x, "WRITE FAILED: Could not lock samples for buffer %s", s_bufname->s_name);
        }
    } else {
        critical_exit(0);
        threads_verbose_log(x, "WRITE SKIPPED: Position %.2f ms out of bounds for buffer %s", bar_ms, s_bufname->s_name);
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
        // Inlet 1: Rescript / Dictionary Reference
        x->audio_dict_name = s;
        threads_rescript(x, s);
    } else if (inlet == 2) {
        // Inlet 2: Palette Configuration
        if (s == gensym("clear")) {
            hashtab_clear(x->palette_map);
            threads_verbose_log(x, "PALETTE MAP CLEARED: All mappings removed");
        } else if (argc >= 1 && (atom_gettype(argv) == A_LONG || atom_gettype(argv) == A_FLOAT)) {
            // Store palette (s) -> index (argv[0])
            t_atom_long index = atom_getlong(argv);
            hashtab_storelong(x->palette_map, s, index);
        }
    } else if (inlet == 0) {
        if (s == gensym("clear") && argc == 0) {
            // Note: This operation is synchronous and blocks the message thread
            // until all buffers in the polybuffer~ have been cleared.
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
                float *samples = NULL;
                int retries = 0;
                for (retries = 0; retries < 10; retries++) {
                    samples = buffer_locksamples(b);
                    if (samples) break;
                    systhread_sleep(1);
                }

                if (samples) {
                    long num_chans = buffer_getchannelcount(b);
                    long num_frames = buffer_getframecount(b);
                    memset(samples, 0, num_frames * num_chans * sizeof(float));
                    buffer_unlocksamples(b);
                    buffer_setdirty(b);
                    critical_exit(0);
                    if (retries > 0) {
                        threads_verbose_log(x, "CLEARED BUFFER: %s [Retries: %d]", bufname, retries);
                    } else {
                        threads_verbose_log(x, "CLEARED BUFFER: %s", bufname);
                    }
                    cleared_count++;
                } else {
                    critical_exit(0);
                    threads_verbose_log(x, "CLEARING FAILED: Could not lock samples for buffer %s", bufname);
                }
                i++;
            }
            if (temp_ref) object_free(temp_ref);

            threads_verbose_log(x, "CLEARING END: Total %d buffers cleared", cleared_count);

            visualize("{\"clear\": 1}");
            threads_verbose_log(x, "CLEAR COMMAND SENT: To visualizer");
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

void threads_dsp64(t_threads *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    if (count[0]) {
        threads_verbose_log(x, "DSP ON: tracking signal ramp");
        dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)threads_perform64, 0, NULL);
    }
}

void threads_perform64(t_threads *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];
    double *out = outs[0];
    double last_val = x->last_ramp_val;
    double last_scan = x->last_scan_val;

    if (critical_tryenter(x->lock) == MAX_ERR_NONE) {
        double initial_scan = last_scan;

        for (int i = 0; i < sampleframes; i++) {
            double current_val = in[i];
            double current_scan = current_val + 9.0; // 9ms lookahead

            out[i] = current_scan; // Output scan head position (ms)

            // 1. DATA hits (integer bar crossings)
            if (last_val != -1.0 && current_val > last_val) {
                long floor_last = (long)floor(last_val);
                long floor_curr = (long)floor(current_val);

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

            // 2. SWEEP tracking (detect wrap-around)
            if (last_scan != -1.0 && current_scan < last_scan) {
                int next_tail = (x->fifo_tail + 1) % 4096;
                if (next_tail != x->fifo_head) {
                    x->hit_bars[x->fifo_tail].bar.value = initial_scan;
                    x->hit_bars[x->fifo_tail].range_end = last_scan;
                    x->hit_bars[x->fifo_tail].type = 1; // SWEEP
                    x->fifo_tail = next_tail;
                }
                initial_scan = current_scan;
            }

            last_val = current_val;
            last_scan = current_scan;
        }

        // Final SWEEP for this vector
        if (last_scan != -1.0 && initial_scan < last_scan) {
            int next_tail = (x->fifo_tail + 1) % 4096;
            if (next_tail != x->fifo_head) {
                x->hit_bars[x->fifo_tail].bar.value = initial_scan;
                x->hit_bars[x->fifo_tail].range_end = last_scan;
                x->hit_bars[x->fifo_tail].type = 1; // SWEEP
                x->fifo_tail = next_tail;
            }
        }

        qelem_set(x->audio_qelem);

        x->last_ramp_val = last_val;
        x->last_scan_val = last_scan;
        critical_exit(x->lock);
    }

}

void threads_audio_qtask(t_threads *x) {
    while (x->fifo_head != x->fifo_tail) {
        t_fifo_entry hit_entry = x->hit_bars[x->fifo_head];
        x->fifo_head = (x->fifo_head + 1) % 4096;

        t_bar_cache hit = hit_entry.bar;
        long hit_type = hit_entry.type; // 0 for DATA, 1 for SWEEP

        if (hit_type == 1) { // SWEEP hit: Constant clear for all tracks
            double start_ms = hit.value;
            double end_ms = hit_entry.range_end;
            long max_t = x->max_tracks;
            if (x->max_track_seen > max_t) max_t = x->max_track_seen;
            if (max_t < 64) max_t = 64; // Safety minimum

            if (start_ms < 0) start_ms = 0;

            for (int track = 1; track <= (int)max_t; track++) {
                char bufname[256];
                snprintf(bufname, 256, "%s.%d", x->poly_prefix->s_name, track);
                t_symbol *s_bufname = gensym(bufname);
                t_buffer_ref *buf_ref = NULL;
                hashtab_lookup(x->buffer_refs, s_bufname, (t_object **)&buf_ref);
                if (!buf_ref) {
                    buf_ref = buffer_ref_new((t_object *)x, s_bufname);
                    hashtab_store(x->buffer_refs, s_bufname, (t_object *)buf_ref);
                }
                t_buffer_obj *b = buffer_ref_getobject(buf_ref);
                if (!b) break; // Contiguous tracks: stop at first missing

                double sr = buffer_getsamplerate(b);
                if (sr <= 0) sr = sys_getsr();
                if (sr <= 0) sr = 44100.0;

                long s_start = (long)round(start_ms * sr / 1000.0);
                long s_end = (long)round(end_ms * sr / 1000.0);
                long n_frames = buffer_getframecount(b);
                long n_chans = buffer_getchannelcount(b);

                if (s_start < 0) s_start = 0;
                if (s_start < n_frames) {
                    if (s_end >= n_frames) s_end = n_frames - 1;
                    if (s_start <= s_end) {
                        float *samples = buffer_locksamples(b);
                        if (samples) {
                            for (long s = s_start; s <= s_end; s++) {
                                for (long c = 0; c < n_chans; c++) {
                                    samples[s * n_chans + c] = 0.0f;
                                }
                            }
                            buffer_unlocksamples(b);
                            buffer_setdirty(b);
                        }
                    }
                }
            }
            continue;
        }

        // DATA Hit logic
        t_symbol *bar_key = hit.sym;
        if (!bar_key) {
            char bstr[64];
            snprintf(bstr, 64, "%ld", (long)hit.value);
            bar_key = gensym(bstr);
        }

        t_dictionary *dict = dictobj_findregistered_retain(x->audio_dict_name);
        if (!dict) {
            threads_verbose_log(x, "ERROR: dictionary '%s' not found", x->audio_dict_name->s_name);
            continue;
        }

        double bar_len = threads_get_bar_length(x);

        for (int track_val = 1; track_val <= x->max_tracks; track_val++) {
            char tstr[64];
            snprintf(tstr, 64, "%d", track_val);
            t_symbol *track_sym = gensym(tstr);
            t_dictionary *track_dict = NULL;

            if (dictionary_getdictionary(dict, track_sym, (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) {
                continue; // Support sparse tracks
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

                threads_process_data(x, palette, (t_atom_long)track_val, hit.value, offset);

                // Silence Cap logic
                t_atomarray *span_aa = NULL;
                if (dictionary_getatomarray(bar_dict, gensym("span"), (t_object **)&span_aa) == MAX_ERR_NONE && span_aa) {
                    long ac_span = 0;
                    t_atom *av_span = NULL;
                    atomarray_getatoms(span_aa, &ac_span, &av_span);
                    if (ac_span > 0) {
                        double max_val = -1.0;
                        for (long j = 0; j < ac_span; j++) {
                            double val = atom_getfloat(av_span + j);
                            if (val > max_val) max_val = val;
                        }
                        double silence_pos = max_val + bar_len;
                        char silence_str[64];
                        snprintf(silence_str, 64, "%ld", (long)silence_pos);
                        t_symbol *silence_sym = gensym(silence_str);

                        // Check if the silence_sym bar exists for THIS track in the dictionary
                        // dict[track_sym][silence_sym]
                        if (!dictionary_hasentry(track_dict, silence_sym)) {
                            threads_process_data(x, gensym("-"), (t_atom_long)track_val, silence_pos, -999999.0);
                        }
                    }
                }
            }
        }
        dictobj_release(dict);
    }
}

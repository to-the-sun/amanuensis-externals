#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "z_dsp.h"
#include "../shared/logging.h"
#include "../shared/visualize.h"
#include <float.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

typedef struct _smartloop {
    t_pxobject s_obj;
    t_symbol *dict_name;
    void *out_start;
    void *out_end;
    void *log_outlet;
    t_clock *clock;
    long interval;
    t_buffer_ref *buffer_ref;
    long bar_warn_sent;
    long log;
    long visualize;
    double current_start;
    double current_end;
    long cached_bar_length;
    double last_val;
    long triggered_zero;
    long first_sample;
    long output_enabled;
    double last_delta;
    double jump_threshold;
    void *out_bang;
    long suppress;
    long suppress_next;
    t_clock *suppress_clock;
    double most_negative_bar;
    double last_most_negative_bar;
    int has_loop;
} t_smartloop;

t_class *smartloop_class;

// Prototypes
void *smartloop_new(t_symbol *s, long argc, t_atom *argv);
void smartloop_free(t_smartloop *x);
void smartloop_calculate(t_smartloop *x);
void smartloop_output_deferred(t_smartloop *x, t_symbol *s, short argc, t_atom *argv);
void smartloop_suppress_tick(t_smartloop *x);
void smartloop_reset_suppress(t_smartloop *x, t_symbol *s, short argc, t_atom *argv);
void smartloop_debug(t_smartloop *x);
void smartloop_int(t_smartloop *x, long n);
t_max_err smartloop_attr_set_log(t_smartloop *x, void *attr, long ac, t_atom *av);
t_max_err smartloop_attr_set_visualize(t_smartloop *x, void *attr, long ac, t_atom *av);
t_max_err smartloop_attr_set_jump_threshold(t_smartloop *x, void *attr, long ac, t_atom *av);
void smartloop_dsp64(t_smartloop *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void smartloop_perform64(t_smartloop *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void smartloop_assist(t_smartloop *x, void *b, long m, long a, char *s);
long smartloop_get_bar_length(t_smartloop *x);
void smartloop_log(t_smartloop *x, const char *fmt, ...);

// Helper for sorting doubles
int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

void smartloop_log(t_smartloop *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "smartloop~", fmt, args);
    va_end(args);
}

long smartloop_get_bar_length(t_smartloop *x) {
    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    if (!b) {
        if (!x->bar_warn_sent) {
            object_warn((t_object *)x, "bar buffer~ not found, attempting to kick reference");
            x->bar_warn_sent = 1;
        }
        buffer_ref_set(x->buffer_ref, _sym_nothing);
        buffer_ref_set(x->buffer_ref, gensym("bar"));
        b = buffer_ref_getobject(x->buffer_ref);
    }
    if (!b) return -1;
    x->bar_warn_sent = 0;

    long bar_length = 0;
    critical_enter(0);
    float *samples = buffer_locksamples(b);
    if (samples) {
        if (buffer_getframecount(b) > 0) {
            bar_length = (long)samples[0];
        }
        buffer_unlocksamples(b);
        critical_exit(0);
    } else {
        critical_exit(0);
    }
    return bar_length;
}

// Robustly get rating from bar dict
double get_rating(t_dictionary *bar_dict) {
    t_atom a;
    if (dictionary_getatom(bar_dict, gensym("rating"), &a) == MAX_ERR_NONE) {
        if (atom_gettype(&a) == A_FLOAT) return atom_getfloat(&a);
        if (atom_gettype(&a) == A_LONG) return (double)atom_getlong(&a);
    }
    t_atomarray *aa = NULL;
    if (dictionary_getatomarray(bar_dict, gensym("rating"), (t_object **)&aa) == MAX_ERR_NONE && aa) {
        long ac; t_atom *av;
        atomarray_getatoms(aa, &ac, &av);
        if (ac > 0) {
            if (atom_gettype(av) == A_FLOAT) return atom_getfloat(av);
            if (atom_gettype(av) == A_LONG) return (double)atom_getlong(av);
        }
    }
    return 0.0;
}

// Robustly get span array from bar dict
t_atomarray* get_span_array(t_dictionary *bar_dict) {
    t_atomarray *aa = NULL;
    if (dictionary_getatomarray(bar_dict, gensym("span"), (t_object **)&aa) == MAX_ERR_NONE && aa) {
        return aa;
    }
    return NULL;
}

void ext_main(void *r) {
    t_class *c = class_new("smartloop~", (method)smartloop_new, (method)smartloop_free, sizeof(t_smartloop), 0L, A_GIMME, 0);
    class_addmethod(c, (method)smartloop_debug, "debug", 0);
    class_addmethod(c, (method)smartloop_int, "int", A_LONG, 0);
    class_addmethod(c, (method)smartloop_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)smartloop_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_smartloop, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");
    CLASS_ATTR_ACCESSORS(c, "log", NULL, (method)smartloop_attr_set_log);

    CLASS_ATTR_LONG(c, "visualize", 0, t_smartloop, visualize);
    CLASS_ATTR_STYLE_LABEL(c, "visualize", 0, "onoff", "Visualize Analysis");
    CLASS_ATTR_DEFAULT(c, "visualize", 0, "1");
    CLASS_ATTR_ACCESSORS(c, "visualize", NULL, (method)smartloop_attr_set_visualize);

    CLASS_ATTR_DOUBLE(c, "jump_threshold", 0, t_smartloop, jump_threshold);
    CLASS_ATTR_LABEL(c, "jump_threshold", 0, "Jump Threshold (ms)");
    CLASS_ATTR_DEFAULT(c, "jump_threshold", 0, "1.0");
    CLASS_ATTR_ACCESSORS(c, "jump_threshold", NULL, (method)smartloop_attr_set_jump_threshold);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    smartloop_class = c;
    common_symbols_init();
}

void *smartloop_new(t_symbol *s, long argc, t_atom *argv) {
    t_smartloop *x = (t_smartloop *)object_alloc(smartloop_class);
    if (x) {
        visualize_init();
        x->dict_name = _sym_nothing;
        x->log = 0;
        x->visualize = 1;

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->dict_name = atom_getsym(argv);
            argc--;
            argv++;
        }

        dsp_setup((t_pxobject *)x, 1);
        attr_args_process(x, argc, argv);

        if (x->dict_name == _sym_nothing) {
            object_error((t_object *)x, "mandatory dictionary name argument missing");
        }

        // Outlets created from right to left
        x->log_outlet = outlet_new(x, NULL); // Index 3
        x->out_end = outlet_new(x, NULL);    // Index 2
        x->out_start = outlet_new(x, NULL);  // Index 1
        x->out_bang = outlet_new(x, NULL);   // Index 0

        x->interval = 999;
        x->bar_warn_sent = 0;
        x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        x->current_start = -1.0;
        x->current_end = -1.0;
        x->cached_bar_length = 0;
        x->last_val = -1.0;
        x->last_delta = 0.0;
        x->jump_threshold = 1.0;
        x->triggered_zero = 0;
        x->first_sample = 1;
        x->output_enabled = 1;
        x->suppress = 0;
        x->suppress_next = 0;
        x->most_negative_bar = 0.0;
        x->last_most_negative_bar = 0.0;
        x->has_loop = 0;

        x->suppress_clock = clock_new(x, (method)smartloop_suppress_tick);
        x->clock = clock_new(x, (method)smartloop_calculate);
        clock_delay(x->clock, x->interval);
    }
    return x;
}

void smartloop_free(t_smartloop *x) {
    visualize_cleanup();
    dsp_free((t_pxobject *)x);
    object_free(x->clock);
    object_free(x->suppress_clock);
    if (x->buffer_ref) object_free(x->buffer_ref);
}

void smartloop_int(t_smartloop *x, long n) {
    x->output_enabled = (n != 0);
    if (x->output_enabled) {
        smartloop_log(x, "Output enabled.");
    } else {
        smartloop_log(x, "Output paused.");
    }
}

t_max_err smartloop_attr_set_log(t_smartloop *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->log = atom_getlong(av);
        smartloop_log(x, "log attribute set to %ld", x->log);
    }
    return MAX_ERR_NONE;
}

t_max_err smartloop_attr_set_visualize(t_smartloop *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->visualize = atom_getlong(av);
        smartloop_log(x, "visualize attribute set to %ld", x->visualize);
    }
    return MAX_ERR_NONE;
}

t_max_err smartloop_attr_set_jump_threshold(t_smartloop *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->jump_threshold = atom_getfloat(av);
        smartloop_log(x, "jump_threshold attribute set to %.2f", x->jump_threshold);
    }
    return MAX_ERR_NONE;
}



void smartloop_suppress_tick(t_smartloop *x) {
    if (x->suppress_next) {
        smartloop_log(x, "Suppression window timed out (99ms). Forgeting next bang.");
    }
    x->suppress_next = 0;
}

void smartloop_output_deferred(t_smartloop *x, t_symbol *s, short argc, t_atom *argv) {
    smartloop_log(x, "Loop/Jump/Start detected. Firing bang (suppress=1). Boundaries: [%.2f, %.2f]",
                 x->current_start, x->current_end);
    if (x->output_enabled) {
        if (x->has_loop) {
            double mapped_start = x->current_start - x->most_negative_bar;
            double mapped_end = x->current_end - x->most_negative_bar;
            outlet_float(x->out_end, mapped_end);
            outlet_float(x->out_start, mapped_start);
        }
    }
    outlet_bang(x->out_bang);

    // After firing outlets, we set suppress_next to 1 to catch the expected
    // "echo" jump caused by the patch's response. If no second bang happens
    // within 99ms, the window closes. 99ms is arbitrary but works in practice.
    x->suppress_next = 1;
    clock_delay(x->suppress_clock, 99);

    x->suppress = 0;
}


void smartloop_reset_suppress(t_smartloop *x, t_symbol *s, short argc, t_atom *argv) {
    if (x->suppress) {
        smartloop_log(x, "Resetting suppression flag (suppress=0).");
    }
    x->suppress = 0;
}

void smartloop_dsp64(t_smartloop *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) {
    dsp_add64(dsp64, (t_object *)x, (t_perfroutine64)smartloop_perform64, 0, NULL);
}

void smartloop_perform64(t_smartloop *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam) {
    double *in = ins[0];
    long do_output_bang = 0;

    // Detect dynamic change in most_negative_bar to prevent false jump detection
    if (x->most_negative_bar != x->last_most_negative_bar) {
        if (!x->first_sample) {
            x->last_val += (x->most_negative_bar - x->last_most_negative_bar);
        }
        x->last_most_negative_bar = x->most_negative_bar;
    }

    // Check if entire vector is stationary relative to last_val.
    // Note: This vector-wide check is a bit arbitrary and it should technically
    // be possible to detect sample by sample, but to improve practical tolerance
    // against false positives, we only consider it stationary if the entire
    // vector is unchanged.
    int all_equal = 1;
    if (!x->first_sample) {
        for (int i = 0; i < sampleframes; i++) {
            double val = in[i] + x->most_negative_bar;
            if (val != x->last_val) {
                all_equal = 0;
                break;
            }
        }
    } else {
        all_equal = 0;
    }

    if (all_equal && !x->first_sample) {
        if (!x->triggered_zero) {
            x->triggered_zero = 1;
            x->last_delta = 0.0;
        }
    } else {
        for (int i = 0; i < sampleframes; i++) {
            double val = in[i] + x->most_negative_bar;

            if (x->first_sample) {
                x->last_val = val;
                x->first_sample = 0;
                continue;
            }

            if (val != x->last_val) {
                double delta = val - x->last_val;
                int jump = (x->last_delta != 0.0 && fabs(delta - x->last_delta) > x->jump_threshold);

                if (val < x->last_val || jump || x->triggered_zero) {
                    do_output_bang = 1;
                }
                x->triggered_zero = 0;
                x->last_delta = delta;
            }
            x->last_val = val;
        }
    }


    if (do_output_bang) {
        if (x->suppress_next) {
            x->suppress_next = 0;
            // The clock is a main-thread object, so we shouldn't manipulate it directly here.
            // However, setting the flag to 0 is enough to break the link.
            smartloop_log(x, "Loop/Jump/Start detected: SUPPRESSING next bang per request.");
        } else if (x->suppress) {
            smartloop_log(x, "Loop/Jump/Start detected but IGNORED due to suppression flag.");
        } else {
            x->suppress = 1;
            smartloop_log(x, "Loop/Jump/Start detected. Triggering deferred output.");
            defer(x, (method)smartloop_output_deferred, NULL, 0, NULL);
        }
    }
}

void smartloop_debug(t_smartloop *x) {
    if (!x->visualize) return;

    if (x->dict_name == _sym_nothing) {
        object_error((t_object *)x, "no dictionary associated");
        return;
    }

    t_dictionary *d = dictobj_findregistered_retain(x->dict_name);
    if (!d) {
        object_error((t_object *)x, "could not find dictionary named %s", x->dict_name->s_name);
        return;
    }

    long num_tracks;
    t_symbol **track_keys = NULL;
    dictionary_getkeys(d, &num_tracks, &track_keys);

    long buf_size = 262144;
    char *json = (char *)sysmem_newptr(buf_size);
    if (!json) {
        if (track_keys) sysmem_freeptr(track_keys);
        dictobj_release(d);
        return;
    }

    long offset = 0;
    int n;
    int first_track = 1;

    n = snprintf(json + offset, buf_size - offset, "{\"event\":\"debug\",\"inventory\":{");
    if (n > 0 && n < buf_size - offset) offset += n;

    for (long i = 0; i < num_tracks && offset < buf_size - 1; i++) {
        t_dictionary *track_dict = NULL;
        if (dictionary_getdictionary(d, track_keys[i], (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) continue;

        if (!first_track && offset < buf_size - 1) {
            json[offset++] = ',';
        }
        first_track = 0;

        n = snprintf(json + offset, buf_size - offset, "\"%s\":{", track_keys[i]->s_name);
        if (n > 0 && n < buf_size - offset) offset += n;

        long num_bars;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        int first_bar = 1;
        for (long j = 0; j < num_bars && offset < buf_size - 1; j++) {
            t_dictionary *bar_dict = NULL;
            if (dictionary_getdictionary(track_dict, bar_keys[j], (t_object **)&bar_dict) != MAX_ERR_NONE || !bar_dict) continue;

            if (!first_bar && offset < buf_size - 1) {
                json[offset++] = ',';
            }
            first_bar = 0;

            double rating = get_rating(bar_dict);
            n = snprintf(json + offset, buf_size - offset, "\"%s\":%.6f", bar_keys[j]->s_name, rating);
            if (n > 0 && n < buf_size - offset) offset += n;
        }

        if (offset < buf_size - 1) {
            json[offset++] = '}';
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }

    if (offset < buf_size - 2) {
        json[offset++] = '}';
        json[offset++] = '}';
        json[offset] = '\0';
    } else if (offset < buf_size) {
        json[offset] = '\0';
    }

    char response[4096];
    int ret = visualize_exchange(x, json, response, sizeof(response));

    if (ret > 0) {
        object_post((t_object *)x, "Debug comparison results: %s", response);
    } else {
        object_error((t_object *)x, "Failed to get response from visualizer for debug comparison.");
    }

    sysmem_freeptr(json);
    if (track_keys) sysmem_freeptr(track_keys);
    dictobj_release(d);
}

void smartloop_assist(t_smartloop *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (signal) Time Ramp (w/ Jump/Start Detection) / (int) Pause/Resume Output (0=pause, 1=resume) / (messages) debug, visualize, jump_threshold, log");
    } else {
        if (a == 0) sprintf(s, "Outlet 1: (bang) Loop/Jump/Start Detected (deferred/safe output)");
        else if (a == 1) sprintf(s, "Outlet 2: (float) Start of longest below average interval (ms)");
        else if (a == 2) sprintf(s, "Outlet 3: (float) End of longest below average interval (ms)");
        else if (a == 3) sprintf(s, "Outlet 4: (anything) Logging Outlet");
    }
}

typedef struct {
    double ts;
    double rating;
} t_aggregated_bar;

// Helper for sorting bars by timestamp
int compare_bars(const void *a, const void *b) {
    double ta = ((t_aggregated_bar *)a)->ts;
    double tb = ((t_aggregated_bar *)b)->ts;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    return 0;
}

void smartloop_calculate(t_smartloop *x) {
    // Periodic polling ensures loop points are pre-calculated so they
    // can be released with minimum latency upon event detection.
    if (x->dict_name == _sym_nothing) {
        clock_delay(x->clock, x->interval);
        return;
    }

    t_dictionary *d = dictobj_findregistered_retain(x->dict_name);
    if (!d) {
        clock_delay(x->clock, x->interval);
        return;
    }

    long num_tracks;
    t_symbol **track_keys = NULL;
    dictionary_getkeys(d, &num_tracks, &track_keys);

    if (num_tracks == 0) {
        if (track_keys) sysmem_freeptr(track_keys);
        dictobj_release(d);
        clock_delay(x->clock, x->interval);
        return;
    }

    long bar_length = smartloop_get_bar_length(x);
    x->cached_bar_length = bar_length;
    if (bar_length <= 0) {
        if (track_keys) sysmem_freeptr(track_keys);
        dictobj_release(d);
        clock_delay(x->clock, x->interval);
        return;
    }

    // Phase 1: Scan all bars from all tracks
    long bar_capacity = 1024;
    t_aggregated_bar *all_bars = (t_aggregated_bar *)sysmem_newptr(bar_capacity * sizeof(t_aggregated_bar));
    long raw_bar_count = 0;
    short has_bars = 0;
    double local_most_negative = 0.0;

    for (long i = 0; i < num_tracks; i++) {
        t_dictionary *track_dict = NULL;
        if (dictionary_getdictionary(d, track_keys[i], (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) continue;

        long num_bars;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        for (long j = 0; j < num_bars; j++) {
            t_dictionary *bar_dict = NULL;
            if (dictionary_getdictionary(track_dict, bar_keys[j], (t_object **)&bar_dict) != MAX_ERR_NONE || !bar_dict) continue;

            double bar_ts = atof(bar_keys[j]->s_name);
            if (bar_ts < local_most_negative) {
                local_most_negative = bar_ts;
            }

            has_bars = 1;
            double rating = get_rating(bar_dict);

            if (raw_bar_count >= bar_capacity) {
                bar_capacity *= 2;
                all_bars = (t_aggregated_bar *)sysmem_resizeptr(all_bars, bar_capacity * sizeof(t_aggregated_bar));
            }
            all_bars[raw_bar_count].ts = bar_ts;
            all_bars[raw_bar_count].rating = rating;
            raw_bar_count++;
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }

    x->most_negative_bar = local_most_negative;

    if (!has_bars) {
        if (all_bars) sysmem_freeptr(all_bars);
        if (track_keys) sysmem_freeptr(track_keys);
        dictobj_release(d);
        clock_delay(x->clock, x->interval);
        return;
    }

    // Phase 2: Aggregate ratings by timestamp
    qsort(all_bars, raw_bar_count, sizeof(t_aggregated_bar), compare_bars);

    t_aggregated_bar *song_bars = (t_aggregated_bar *)sysmem_newptr(raw_bar_count * sizeof(t_aggregated_bar));
    long song_bar_count = 0;
    double global_rating_sum = 0.0;
    double min_rating = DBL_MAX;
    double max_rating = -DBL_MAX;

    if (raw_bar_count > 0) {
        double current_ts = all_bars[0].ts;
        double current_sum = all_bars[0].rating;
        int current_count = 1;

        for (long i = 1; i < raw_bar_count; i++) {
            if (fabs(all_bars[i].ts - current_ts) < 0.001) { // Floating point equality check
                current_sum += all_bars[i].rating;
                current_count++;
            } else {
                double avg_rating = current_sum / current_count;
                song_bars[song_bar_count].ts = current_ts;
                song_bars[song_bar_count].rating = avg_rating;
                global_rating_sum += avg_rating;
                if (avg_rating < min_rating) min_rating = avg_rating;
                if (avg_rating > max_rating) max_rating = avg_rating;
                song_bar_count++;

                current_ts = all_bars[i].ts;
                current_sum = all_bars[i].rating;
                current_count = 1;
            }
        }
        double avg_rating = current_sum / current_count;
        song_bars[song_bar_count].ts = current_ts;
        song_bars[song_bar_count].rating = avg_rating;
        global_rating_sum += avg_rating;
        if (avg_rating < min_rating) min_rating = avg_rating;
        if (avg_rating > max_rating) max_rating = avg_rating;
        song_bar_count++;
    }

    double average = (song_bar_count > 0) ? (global_rating_sum / song_bar_count) : 0.0;

    smartloop_log(x, "Analysis: %ld raw bars, %ld unique timestamps. Ratings: min=%.2f, max=%.2f, global_avg=%.2f",
                  raw_bar_count, song_bar_count, min_rating, max_rating, average);

    // Phase 3: Identify boundaries and potential intervals
    double *above_avg_points = (double *)sysmem_newptr(song_bar_count * sizeof(double));
    long unique_above_avg_count = 0;

    double *below_avg_starts = (double *)sysmem_newptr(song_bar_count * sizeof(double));
    double *below_avg_ends = (double *)sysmem_newptr(song_bar_count * sizeof(double));
    long below_avg_count_total = 0;

    for (long i = 0; i < song_bar_count; i++) {
        if (song_bars[i].rating > average) {
            above_avg_points[unique_above_avg_count++] = song_bars[i].ts;
        } else {
            below_avg_starts[below_avg_count_total] = song_bars[i].ts;
            below_avg_ends[below_avg_count_total] = song_bars[i].ts + bar_length;
            below_avg_count_total++;
        }
    }

    // Phase 4: Longest clean interval
    double max_dist = -1.0;
    double best_S = 0.0;
    double best_E = 0.0;

    double overall_min = DBL_MAX;
    double overall_max = -DBL_MAX;
    for (long i = 0; i < song_bar_count; i++) {
        if (song_bars[i].ts < overall_min) overall_min = song_bars[i].ts;
        if (song_bars[i].ts > overall_max) overall_max = song_bars[i].ts;
    }
    overall_max += bar_length;

    double *bounded_above_avg = (double *)sysmem_newptr((unique_above_avg_count + 2) * sizeof(double));
    bounded_above_avg[0] = overall_min - 1.0;
    for (long i = 0; i < unique_above_avg_count; i++) bounded_above_avg[i+1] = above_avg_points[i];
    bounded_above_avg[unique_above_avg_count + 1] = overall_max + 1.0;

    for (long i = 0; i < unique_above_avg_count + 1; i++) {
        double p_low = bounded_above_avg[i];
        double p_high = bounded_above_avg[i+1];

        double cur_min_S = DBL_MAX;
        double cur_max_E = -DBL_MAX;
        short found_below_avg = 0;

        for (long j = 0; j < below_avg_count_total; j++) {
            if (below_avg_starts[j] >= p_low && below_avg_ends[j] <= p_high) {
                if (below_avg_starts[j] < cur_min_S) cur_min_S = below_avg_starts[j];
                if (below_avg_ends[j] > cur_max_E) cur_max_E = below_avg_ends[j];
                found_below_avg = 1;
            }
        }

        if (found_below_avg) {
            double dist = cur_max_E - cur_min_S;
            if (dist > max_dist) {
                max_dist = dist;
                best_S = cur_min_S;
                best_E = cur_max_E;
            }
        }
    }

    if (max_dist >= 0.0) {
        if (best_S == 0.0) best_S = 1.0;
        if (best_E == 0.0) best_E = 1.0;
        smartloop_log(x, "Loop identified: start=%.2f, end=%.2f, duration=%.2f", best_S, best_E, max_dist);
        x->current_start = best_S;
        x->current_end = best_E;
        x->has_loop = 1;
    } else {
        smartloop_log(x, "No valid loop found.");
        x->current_start = -1.0;
        x->current_end = -1.0;
        x->has_loop = 0;
    }

    sysmem_freeptr(bounded_above_avg);
    sysmem_freeptr(below_avg_starts);
    sysmem_freeptr(below_avg_ends);
    sysmem_freeptr(above_avg_points);
    sysmem_freeptr(all_bars);
    sysmem_freeptr(song_bars);
    if (track_keys) sysmem_freeptr(track_keys);
    dictobj_release(d);

    clock_delay(x->clock, x->interval);
}

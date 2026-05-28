#include "ext.h"
#include "ext_obex.h"
#include "ext_dictobj.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "ext_systhread.h"
#include "../shared/logging.h"
#include <float.h>
#include <stdlib.h>
#include <math.h>

typedef struct _smartloop {
    t_object s_obj;
    t_symbol *dict_name;
    void *out_start;
    void *out_end;
    void *log_outlet;
    t_clock *clock;
    long interval;
    t_buffer_ref *buffer_ref;
    long bar_warn_sent;
    long log;
} t_smartloop;

t_class *smartloop_class;

// Prototypes
void *smartloop_new(t_symbol *s, long argc, t_atom *argv);
void smartloop_free(t_smartloop *x);
void smartloop_tick(t_smartloop *x);
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
    vcommon_log(x->log_outlet, x->log, "smartloop", fmt, args);
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
    t_class *c = class_new("smartloop", (method)smartloop_new, (method)smartloop_free, sizeof(t_smartloop), 0L, A_GIMME, 0);
    class_addmethod(c, (method)smartloop_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_smartloop, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    smartloop_class = c;
    common_symbols_init();
}

void *smartloop_new(t_symbol *s, long argc, t_atom *argv) {
    t_smartloop *x = (t_smartloop *)object_alloc(smartloop_class);
    if (x) {
        x->dict_name = _sym_nothing;
        x->log = 0;

        if (argc > 0 && atom_gettype(argv) == A_SYM && strncmp(atom_getsym(argv)->s_name, "@", 1) != 0) {
            x->dict_name = atom_getsym(argv);
            argc--;
            argv++;
        }

        attr_args_process(x, argc, argv);

        if (x->dict_name == _sym_nothing) {
            object_error((t_object *)x, "mandatory dictionary name argument missing");
        }

        // Outlets created from right to left
        x->log_outlet = outlet_new(x, NULL); // Index 2
        x->out_end = outlet_new(x, NULL);    // Index 1
        x->out_start = outlet_new(x, NULL);  // Index 0

        x->interval = 999;
        x->bar_warn_sent = 0;
        x->buffer_ref = buffer_ref_new((t_object *)x, gensym("bar"));

        x->clock = clock_new(x, (method)smartloop_tick);
        clock_delay(x->clock, x->interval);
    }
    return x;
}

void smartloop_free(t_smartloop *x) {
    object_free(x->clock);
    if (x->buffer_ref) object_free(x->buffer_ref);
}

void smartloop_assist(t_smartloop *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: Unused (Object scans dictionary every 999ms)");
    } else {
        if (a == 0) sprintf(s, "Outlet 1: Start of longest below average interval (ms)");
        else if (a == 1) sprintf(s, "Outlet 2: End of longest below average interval (ms)");
        else if (a == 2) sprintf(s, "Outlet 3: Logging Outlet");
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

void smartloop_tick(t_smartloop *x) {
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

    for (long i = 0; i < num_tracks; i++) {
        t_dictionary *track_dict = NULL;
        if (dictionary_getdictionary(d, track_keys[i], (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) continue;

        long num_bars;
        t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);

        for (long j = 0; j < num_bars; j++) {
            t_dictionary *bar_dict = NULL;
            if (dictionary_getdictionary(track_dict, bar_keys[j], (t_object **)&bar_dict) != MAX_ERR_NONE || !bar_dict) continue;

            has_bars = 1;
            double rating = get_rating(bar_dict);
            double bar_ts = atof(bar_keys[j]->s_name);

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

    smartloop_log(x, "Avoiding %ld unique above-average points.", unique_above_avg_count);

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
            if (cur_min_S < 0) cur_min_S = 0;
            double dist = cur_max_E - cur_min_S;
            if (dist > max_dist) {
                max_dist = dist;
                best_S = cur_min_S;
                best_E = cur_max_E;
            }
        }
    }

    if (max_dist >= 0.0) {
        smartloop_log(x, "Loop identified: start=%.2f, end=%.2f, duration=%.2f", best_S, best_E, max_dist);
        outlet_float(x->out_end, best_E);
        outlet_float(x->out_start, best_S);
    } else {
        smartloop_log(x, "No valid loop found.");
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

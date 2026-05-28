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
        if (a == 0) sprintf(s, "Outlet 1: Start of longest Group 2 interval (ms)");
        else if (a == 1) sprintf(s, "Outlet 2: End of longest Group 2 interval (ms)");
        else if (a == 2) sprintf(s, "Outlet 3: Logging Outlet");
    }
}

typedef struct {
    double rating;
    double start;
    double end_bar;
} t_span_info;

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

    // Phase 1: Scan all bars and spans
    long bar_capacity = 1024;
    double *all_bar_timestamps = (double *)sysmem_newptr(bar_capacity * sizeof(double));
    long bar_count = 0;

    long span_capacity = 256;
    t_span_info *spans = (t_span_info *)sysmem_newptr(span_capacity * sizeof(t_span_info));
    long span_count = 0;

    double min_rating = DBL_MAX;
    double max_rating = -DBL_MAX;
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
            if (rating < min_rating) min_rating = rating;
            if (rating > max_rating) max_rating = rating;

            double bar_ts = atof(bar_keys[j]->s_name);
            if (bar_count >= bar_capacity) {
                bar_capacity *= 2;
                all_bar_timestamps = (double *)sysmem_resizeptr(all_bar_timestamps, bar_capacity * sizeof(double));
            }
            all_bar_timestamps[bar_count++] = bar_ts;

            t_atomarray *span_aa = get_span_array(bar_dict);
            if (span_aa) {
                long ac; t_atom *av;
                atomarray_getatoms(span_aa, &ac, &av);
                if (ac > 0) {
                    double s_min = DBL_MAX;
                    double s_max = -DBL_MAX;
                    for (long k = 0; k < ac; k++) {
                        double ts = (atom_gettype(av+k) == A_LONG) ? (double)atom_getlong(av+k) : atom_getfloat(av+k);
                        if (ts < s_min) s_min = ts;
                        if (ts > s_max) s_max = ts;
                    }

                    short found = 0;
                    for (long k = 0; k < span_count; k++) {
                        if (spans[k].start == s_min && spans[k].end_bar == s_max) {
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        if (span_count >= span_capacity) {
                            span_capacity *= 2;
                            spans = (t_span_info *)sysmem_resizeptr(spans, span_capacity * sizeof(t_span_info));
                        }
                        spans[span_count].rating = rating;
                        spans[span_count].start = s_min;
                        spans[span_count].end_bar = s_max;
                        span_count++;
                    }
                }
            } else {
                short found = 0;
                for (long k = 0; k < span_count; k++) {
                    if (spans[k].start == bar_ts && spans[k].end_bar == bar_ts) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    if (span_count >= span_capacity) {
                        span_capacity *= 2;
                        spans = (t_span_info *)sysmem_resizeptr(spans, span_capacity * sizeof(t_span_info));
                    }
                    spans[span_count].rating = rating;
                    spans[span_count].start = bar_ts;
                    spans[span_count].end_bar = bar_ts;
                    span_count++;
                }
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }

    if (!has_bars) {
        sysmem_freeptr(all_bar_timestamps);
        sysmem_freeptr(spans);
        if (track_keys) sysmem_freeptr(track_keys);
        dictobj_release(d);
        clock_delay(x->clock, x->interval);
        return;
    }

    double midpoint = (min_rating + max_rating) / 2.0;

    // Collect Group 1 points
    double *points1 = (double *)sysmem_newptr(bar_count * sizeof(double));
    long p1_count = 0;

    for (long i = 0; i < num_tracks; i++) {
        t_dictionary *track_dict = NULL;
        if (dictionary_getdictionary(d, track_keys[i], (t_object **)&track_dict) != MAX_ERR_NONE || !track_dict) continue;
        long num_bars; t_symbol **bar_keys = NULL;
        dictionary_getkeys(track_dict, &num_bars, &bar_keys);
        for (long j = 0; j < num_bars; j++) {
            t_dictionary *bar_dict = NULL;
            if (dictionary_getdictionary(track_dict, bar_keys[j], (t_object **)&bar_dict) != MAX_ERR_NONE || !bar_dict) continue;
            if (get_rating(bar_dict) > midpoint) {
                points1[p1_count++] = atof(bar_keys[j]->s_name);
            }
        }
        if (bar_keys) sysmem_freeptr(bar_keys);
    }
    qsort(points1, p1_count, sizeof(double), compare_doubles);
    long unique_p1_count = 0;
    for (long i = 0; i < p1_count; i++) {
        if (i == 0 || points1[i] != points1[i-1]) {
            points1[unique_p1_count++] = points1[i];
        }
    }

    // Group 2 spans
    double *g2_starts = (double *)sysmem_newptr(span_count * sizeof(double));
    double *g2_ends = (double *)sysmem_newptr(span_count * sizeof(double));
    long g2_count = 0;
    for (long i = 0; i < span_count; i++) {
        if (spans[i].rating < midpoint) {
            g2_starts[g2_count] = spans[i].start;
            g2_ends[g2_count] = spans[i].end_bar + bar_length;
            g2_count++;
        }
    }

    // Longest clean interval
    double max_dist = -1.0;
    double best_S = 0.0;
    double best_E = 0.0;

    double overall_min = DBL_MAX;
    double overall_max = -DBL_MAX;
    for (long i = 0; i < bar_count; i++) {
        if (all_bar_timestamps[i] < overall_min) overall_min = all_bar_timestamps[i];
        if (all_bar_timestamps[i] > overall_max) overall_max = all_bar_timestamps[i];
    }
    overall_max += bar_length;

    double *bounded_p1 = (double *)sysmem_newptr((unique_p1_count + 2) * sizeof(double));
    bounded_p1[0] = overall_min - 1.0;
    for (long i = 0; i < unique_p1_count; i++) bounded_p1[i+1] = points1[i];
    bounded_p1[unique_p1_count + 1] = overall_max + 1.0;

    for (long i = 0; i < unique_p1_count + 1; i++) {
        double p_low = bounded_p1[i];
        double p_high = bounded_p1[i+1];

        double cur_min_S = DBL_MAX;
        double cur_max_E = -DBL_MAX;
        short found_g2 = 0;

        for (long j = 0; j < g2_count; j++) {
            if (g2_starts[j] >= p_low && g2_ends[j] <= p_high) {
                if (g2_starts[j] < cur_min_S) cur_min_S = g2_starts[j];
                if (g2_ends[j] > cur_max_E) cur_max_E = g2_ends[j];
                found_g2 = 1;
            }
        }

        if (found_g2) {
            double dist = cur_max_E - cur_min_S;
            if (dist > max_dist) {
                max_dist = dist;
                best_S = cur_min_S;
                best_E = cur_max_E;
            }
        }
    }

    if (max_dist >= 0.0) {
        outlet_float(x->out_end, best_E);
        outlet_float(x->out_start, best_S);
    }

    sysmem_freeptr(bounded_p1);
    sysmem_freeptr(g2_starts);
    sysmem_freeptr(g2_ends);
    sysmem_freeptr(points1);
    sysmem_freeptr(all_bar_timestamps);
    sysmem_freeptr(spans);
    if (track_keys) sysmem_freeptr(track_keys);
    dictobj_release(d);

    clock_delay(x->clock, x->interval);
}

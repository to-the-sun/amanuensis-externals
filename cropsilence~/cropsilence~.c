#include "ext.h"
#include "ext_obex.h"
#include "ext_buffer.h"
#include "ext_critical.h"
#include "../shared/logging.h"
#include <string.h>
#include <math.h>

typedef struct _cropsilence {
    t_object b_obj;
    t_symbol *poly_name;
    long log;
    void *log_outlet;
    void *status_outlet;
} t_cropsilence;

void *cropsilence_new(t_symbol *s, long argc, t_atom *argv);
void cropsilence_free(t_cropsilence *x);
void cropsilence_int(t_cropsilence *x, t_atom_long n);
void cropsilence_float(t_cropsilence *x, double f);
void cropsilence_assist(t_cropsilence *x, void *b, long m, long a, char *s);

void cropsilence_log(t_cropsilence *x, const char *fmt, ...);
void cropsilence_execute(t_cropsilence *x, double bar_ms);

static t_class *cropsilence_class;

void ext_main(void *r) {
    common_symbols_init();

    t_class *c = class_new("cropsilence~", (method)cropsilence_new, (method)cropsilence_free, sizeof(t_cropsilence), 0L, A_GIMME, 0);

    class_addmethod(c, (method)cropsilence_int, "int", A_LONG, 0);
    class_addmethod(c, (method)cropsilence_float, "float", A_FLOAT, 0);
    class_addmethod(c, (method)cropsilence_assist, "assist", A_CANT, 0);

    CLASS_ATTR_LONG(c, "log", 0, t_cropsilence, log);
    CLASS_ATTR_STYLE_LABEL(c, "log", 0, "onoff", "Enable Logging");
    CLASS_ATTR_DEFAULT(c, "log", 0, "0");

    class_register(CLASS_BOX, c);
    cropsilence_class = c;
}

void cropsilence_log(t_cropsilence *x, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vcommon_log(x->log_outlet, x->log, "cropsilence~", fmt, args);
    va_end(args);
}

void *cropsilence_new(t_symbol *s, long argc, t_atom *argv) {
    t_cropsilence *x = (t_cropsilence *)object_alloc(cropsilence_class);

    if (x) {
        x->poly_name = _sym_nothing;
        x->log = 0;

        // Process Arguments
        if (argc > 0 && atom_gettype(argv) == A_SYM && atom_getsym(argv)->s_name[0] != '@') {
            x->poly_name = atom_getsym(argv);
            argc--;
            argv++;
        } else {
            object_error((t_object *)x, "missing mandatory polybuffer~ name argument");
        }

        attr_args_process(x, argc, argv);

        // Create outlets from right to left
        x->log_outlet = outlet_new((t_object *)x, NULL);
        x->status_outlet = outlet_new((t_object *)x, NULL);

        // Verify existence of name.1
        if (x->poly_name != _sym_nothing) {
            char bufname[256];
            snprintf(bufname, 256, "%s.1", x->poly_name->s_name);
            t_buffer_ref *temp = buffer_ref_new((t_object *)x, gensym(bufname));
            if (!buffer_ref_getobject(temp)) {
                object_warn((t_object *)x, "could not find initial buffer '%s'", bufname);
            }
            object_free(temp);
        }
    }
    return x;
}

void cropsilence_free(t_cropsilence *x) {
    // Nothing special to free
}

void cropsilence_int(t_cropsilence *x, t_atom_long n) {
    cropsilence_execute(x, (double)n);
}

void cropsilence_float(t_cropsilence *x, double f) {
    cropsilence_execute(x, f);
}

void cropsilence_execute(t_cropsilence *x, double bar_ms) {
    if (bar_ms <= 0) {
        object_error((t_object *)x, "invalid bar length: %f", bar_ms);
        return;
    }

    if (x->poly_name == _sym_nothing) {
        object_error((t_object *)x, "no polybuffer~ name specified");
        return;
    }

    cropsilence_log(x, "START: Cropping polybuffer~ '%s' with bar length %.2f ms", x->poly_name->s_name, bar_ms);

    char bufname[256];
    int buffer_index = 1;
    snprintf(bufname, 256, "%s.%d", x->poly_name->s_name, buffer_index);
    t_symbol *s_member = gensym(bufname);
    t_buffer_ref *b_ref = buffer_ref_new((t_object *)x, s_member);

    while (buffer_ref_getobject(b_ref)) {
        t_buffer_obj *b = buffer_ref_getobject(b_ref);
        double sr = buffer_getsamplerate(b);
        long long total_frames = buffer_getframecount(b);
        long chans = buffer_getchannelcount(b);

        if (sr > 0 && total_frames > 0 && chans > 0) {
            long long bar_frames = (long long)ceil((bar_ms * sr) / 1000.0);
            if (bar_frames <= 0) {
                cropsilence_log(x, "SKIPPED: %s (bar length too short for sample rate)", s_member->s_name);
                goto next_buffer;
            }

            float *samples = buffer_locksamples(b);
            if (!samples) {
                cropsilence_log(x, "ERROR: Could not lock samples for %s", s_member->s_name);
                goto next_buffer;
            }

            long long num_bars = (total_frames + bar_frames - 1) / bar_frames;
            int *keep_bar = (int *)sysmem_newptr(num_bars * sizeof(int));
            long long kept_count = 0;

            for (long long i = 0; i < num_bars; i++) {
                long long start_frame = i * bar_frames;
                long long end_frame = start_frame + bar_frames;
                if (end_frame > total_frames) end_frame = total_frames;

                int has_audio = 0;
                for (long long f = start_frame; f < end_frame; f++) {
                    for (long c = 0; c < chans; c++) {
                        if (fabs(samples[f * chans + c]) > 0.00009) {
                            has_audio = 1;
                            break;
                        }
                    }
                    if (has_audio) break;
                }

                if (has_audio) {
                    keep_bar[i] = 1;
                    kept_count++;
                } else {
                    keep_bar[i] = 0;
                }
            }

            if (kept_count < num_bars) {
                cropsilence_log(x, "%s: Found %lld silent bars out of %lld. Cropping...", s_member->s_name, num_bars - kept_count, num_bars);

                float *new_samples = (float *)sysmem_newptr(kept_count * bar_frames * chans * sizeof(float));
                if (new_samples) {
                    memset(new_samples, 0, kept_count * bar_frames * chans * sizeof(float));
                    long long current_kept_bar = 0;
                    for (long long i = 0; i < num_bars; i++) {
                        if (keep_bar[i]) {
                            long long src_start = i * bar_frames;
                            long long dst_start = current_kept_bar * bar_frames;
                            long long frames_to_copy = bar_frames;
                            if (src_start + frames_to_copy > total_frames) {
                                frames_to_copy = total_frames - src_start;
                            }
                            memcpy(new_samples + (dst_start * chans), samples + (src_start * chans), frames_to_copy * chans * sizeof(float));
                            current_kept_bar++;
                        }
                    }
                    buffer_unlocksamples(b);

                    // Resize and copy back
                    long long new_total_frames = kept_count * bar_frames;
                    buffer_edit_begin(b);
                    t_atom av;
                    atom_setlong(&av, (t_atom_long)new_total_frames);
                    object_method_typed(b, gensym("sizeinsamps"), 1, &av, NULL);

                    float *res_samples = buffer_locksamples(b);
                    if (res_samples) {
                        // Re-verify channel count after resize just in case
                        long new_chans = buffer_getchannelcount(b);
                        if (new_chans == chans) {
                            memcpy(res_samples, new_samples, new_total_frames * chans * sizeof(float));
                        } else {
                            cropsilence_log(x, "ERROR: Channel count changed after resize for %s", s_member->s_name);
                        }
                        buffer_unlocksamples(b);
                    } else {
                        cropsilence_log(x, "ERROR: Could not lock samples after resize for %s", s_member->s_name);
                    }
                    buffer_edit_end(b, 1);
                    buffer_setdirty(b);
                    sysmem_freeptr(new_samples);
                } else {
                    cropsilence_log(x, "ERROR: Memory allocation failed for cropping %s", s_member->s_name);
                    buffer_unlocksamples(b);
                }
            } else {
                cropsilence_log(x, "%s: No silent bars found.", s_member->s_name);
                buffer_unlocksamples(b);
            }

            sysmem_freeptr(keep_bar);
        } else {
            cropsilence_log(x, "SKIPPED: %s (invalid buffer parameters)", s_member->s_name);
        }

    next_buffer:
        buffer_index++;
        snprintf(bufname, 256, "%s.%d", x->poly_name->s_name, buffer_index);
        s_member = gensym(bufname);
        buffer_ref_set(b_ref, s_member);
    }

    object_free(b_ref);
    cropsilence_log(x, "END: Finished cropping polybuffer~ '%s'", x->poly_name->s_name);
    outlet_bang(x->status_outlet);
}

void cropsilence_assist(t_cropsilence *x, void *b, long m, long a, char *s) {
    if (m == ASSIST_INLET) {
        sprintf(s, "Inlet 1: (number) Trigger cropping with specified bar length (ms)");
    } else {
        switch (a) {
            case 0: sprintf(s, "Outlet 1: Bang when done"); break;
            case 1: sprintf(s, "Outlet 2: Logging Outlet"); break;
        }
    }
}

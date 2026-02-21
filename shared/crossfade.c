#include "crossfade.h"
#include <math.h>

#define HI (44.0 * 4999.0)
#define LO 999.0

void ramp_init(t_ramp_state *x) {
    x->length = HI;
    x->toggle = 0.0;
    x->go = 0.0;
    x->last_amp = 0.0;
}

double ramp_process(t_ramp_state *x, double signal, double movement, long long elapsed, double *fade_out) {
    if (movement != 0.0) {
        x->length = HI;
        x->go = (double)elapsed;
        x->toggle = (movement > 0.0) ? 1.0 : 0.0;
    }

    double abs_sig = fabs(signal);
    // slide(abs_sig, 0, LO)
    // up=0 (no smoothing), down=LO
    double amp;
    if (abs_sig > x->last_amp) {
        amp = abs_sig; // up=0 means immediate
    } else {
        amp = x->last_amp + (abs_sig - x->last_amp) / LO;
    }
    x->last_amp = amp;

    // length = clip(amp * hi, lo, length)
    double target_length = amp * HI;
    if (target_length < LO) target_length = LO;
    if (target_length > x->length) target_length = x->length;
    x->length = target_length;

    double age = (double)elapsed - x->go;
    double fade = age / x->length;
    if (fade > 1.0) fade = 1.0;
    if (fade < 0.0) fade = 0.0;

    fade = fabs(x->toggle - fade);

    *fade_out = fade;
    return signal * fade;
}

void crossfade_init(t_crossfade_state *x) {
    ramp_init(&x->ramp1);
    ramp_init(&x->ramp2);
    x->direction = -1.0;
    x->last_control = 0.0;
    x->elapsed = 0;
}

void crossfade_process(t_crossfade_state *x, double control, double s1, double s2, double *mix1, double *mix2, double *sum, int *busy) {
    double f1, f2;
    *mix1 = ramp_process(&x->ramp1, s1, x->direction, x->elapsed, &f1);
    *mix2 = ramp_process(&x->ramp2, s2, x->direction * -1.0, x->elapsed, &f2);

    // finished = !(fader1 % 1) && !(fader2 % 1);
    // Gen's ! (f % 1) is true if f is integer.
    int finished = (f1 <= 0.0 || f1 >= 1.0) && (f2 <= 0.0 || f2 >= 1.0);

    x->direction = 0.0;
    if (finished) {
        // direction = change(control)
        double diff = control - x->last_control;
        if (diff != 0.0) {
            x->direction = diff;
        }
    }
    x->last_control = control;

    *sum = *mix1 + *mix2;
    *busy = !finished;
    x->elapsed++;
}

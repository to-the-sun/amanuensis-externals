#ifndef _SHARED_DSP_H_
#define _SHARED_DSP_H_

#include "ext.h"

typedef struct _ramp_state {
    double length;
    double toggle;
    double go;
    double last_amp;
} t_ramp_state;

typedef struct _crossfade_state {
    t_ramp_state ramp1;
    t_ramp_state ramp2;
    double direction;
    double last_control;
    long long elapsed;
} t_crossfade_state;

void ramp_init(t_ramp_state *x);
double ramp_process(t_ramp_state *x, double signal, double movement, long long elapsed, double *fade_out);

void crossfade_init(t_crossfade_state *x);
void crossfade_process(t_crossfade_state *x, double control, double s1, double s2, double *mix1, double *mix2, double *sum, int *busy);

#endif

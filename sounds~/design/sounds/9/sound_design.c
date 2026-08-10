#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double start_time;
    int velocity;
    int active;
} ActiveNote;

typedef struct {
    double x1, x2, y1, y2;
    double b0, b1, b2, a1, a2;
} Biquad;

static void update_lpf(Biquad* f, double cutoff, double Q, int sample_rate) {
    double omega = 2.0 * M_PI * cutoff / sample_rate;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);

    double a0 = 1.0 + alpha;
    f->b0 = ((1.0 - cs) / 2.0) / a0;
    f->b1 = (1.0 - cs) / a0;
    f->b2 = ((1.0 - cs) / 2.0) / a0;
    f->a1 = (-2.0 * cs) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

static void init_biquad(Biquad* f) {
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0;
    f->b0 = f->b1 = f->b2 = f->a1 = f->a2 = 0.0;
}

static void adsr_envelope(double* buffer, int duration_samples, int attack_samples, int decay_samples, double sustain_level, int release_samples, int is_sustained) {
    int a_len = attack_samples;
    int d_len = decay_samples;
    int r_len = is_sustained ? 0 : release_samples;
    int total_adr = a_len + d_len + r_len;
    int s_len;
    if (duration_samples < total_adr && total_adr > 0) {
        double scale = (double)duration_samples / total_adr;
        a_len = (int)(a_len * scale);
        d_len = (int)(d_len * scale);
        r_len = (int)(r_len * scale);
        s_len = 0;
    } else {
        s_len = duration_samples - a_len - d_len - r_len;
    }
    int current = 0;
    for (int i = 0; i < a_len && current < duration_samples; i++, current++) buffer[current] = (double)i / a_len;
    for (int i = 0; i < d_len && current < duration_samples; i++, current++) buffer[current] = 1.0 - (1.0 - sustain_level) * ((double)i / d_len);
    for (int i = 0; i < s_len && current < duration_samples; i++, current++) buffer[current] = sustain_level;
    if (!is_sustained) for (int i = 0; i < r_len && current < duration_samples; i++, current++) buffer[current] = sustain_level * (1.0 - (double)i / r_len);
    while (current < duration_samples) buffer[current++] = is_sustained ? sustain_level : 0.0;
}

static void render_note(double* output, int num_samples, int note_num, double start_time, double end_time, int velocity, int sample_rate, int is_sustained) {
    double freq = 440.0 * pow(2.0, (note_num - 69) / 12.0);

    double attack_time = 0.05;
    double decay_time = 0.3;
    double sustain_level = 0.6;
    double release_time = 0.5;

    double note_duration = is_sustained ? (end_time - start_time) : (end_time - start_time + release_time);
    int note_samples = (int)(note_duration * sample_rate);
    int start_idx = (int)(start_time * sample_rate);
    int end_idx = start_idx + note_samples;
    if (end_idx > num_samples) end_idx = num_samples;
    int actual_samples = end_idx - start_idx;
    if (actual_samples <= 0) return;

    double* env = (double*)malloc(actual_samples * sizeof(double));
    adsr_envelope(env, actual_samples, (int)(attack_time * sample_rate), (int)(decay_time * sample_rate), sustain_level, (int)(release_time * sample_rate), is_sustained);

    // Initial detuned phases
    double phases[5] = {0.0, 0.2, 0.4, 0.6, 0.8};
    double sub_phase = 0.0;

    Biquad filter;
    init_biquad(&filter);

    double osc_amps[5] = {0.18, 0.22, 0.3, 0.22, 0.18};
    double osc_detunes[5] = {0.990, 0.995, 1.0, 1.005, 1.010};

    // Use deterministic seed for noise consistency
    srand(note_num);

    for (int i = 0; i < actual_samples; i++) {
        double t_local = (double)i / sample_rate;

        // Sum the 5 detuned sawtooth wave oscillators
        double mix = 0.0;
        for (int osc = 0; osc < 5; osc++) {
            phases[osc] += (freq * osc_detunes[osc]) / sample_rate;
            if (phases[osc] >= 1.0) phases[osc] -= 1.0;
            mix += (2.0 * phases[osc] - 1.0) * osc_amps[osc];
        }

        // Sub oscillator at half frequency
        sub_phase += (freq * 0.5) / sample_rate;
        if (sub_phase >= 1.0) sub_phase -= 1.0;
        double sub_osc = sin(2.0 * M_PI * sub_phase);

        // Noise component
        double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0) * 0.15;

        // Raw mix
        double raw_signal = mix * 0.7 + sub_osc * 0.2 + noise * 0.1;

        // Dynamic Lowpass Filter Sweep
        double cutoff_start = freq * 6.5;
        double cutoff_end = freq * 1.5;
        if (cutoff_start > sample_rate * 0.45) cutoff_start = sample_rate * 0.45;
        if (cutoff_end > sample_rate * 0.45) cutoff_end = sample_rate * 0.45;
        if (cutoff_end < 80.0) cutoff_end = 80.0;

        double current_cutoff = cutoff_end + (cutoff_start - cutoff_end) * exp(-t_local / 0.35);
        update_lpf(&filter, current_cutoff, 2.2, sample_rate);

        // Process biquad filter
        double filtered = raw_signal;
        // Biquad standard equation processing
        double y = filter.b0 * filtered + filter.b1 * filter.x1 + filter.b2 * filter.x2 - filter.a1 * filter.y1 - filter.a2 * filter.y2;
        filter.x2 = filter.x1;
        filter.x1 = filtered;
        filter.y2 = filter.y1;
        filter.y1 = y;
        filtered = y;

        // Apply envelope and velocity
        // Initial test gain is 1.0, to be calibrated in Step 3
        double current_gain = 1.001565103444;
        output[start_idx + i] += filtered * env[i] * (velocity / 127.0) * current_gain;
    }
    free(env);
}

double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out) {
    int num_samples = (int)(duration * sample_rate);
    *num_samples_out = num_samples;
    double* output = (double*)calloc(num_samples, sizeof(double));
    ActiveNote active_notes[128] = {0};
    for (int i = 0; i < num_messages; i++) {
        MidiMessage msg = midi_messages[i];
        if (msg.time >= duration) continue;
        if (strcmp(msg.type, "note_on") == 0 && msg.velocity > 0) {
            active_notes[msg.note].start_time = msg.time;
            active_notes[msg.note].velocity = msg.velocity;
            active_notes[msg.note].active = 1;
        } else if (strcmp(msg.type, "note_off") == 0 || (strcmp(msg.type, "note_on") == 0 && msg.velocity == 0)) {
            if (active_notes[msg.note].active) {
                render_note(output, num_samples, msg.note, active_notes[msg.note].start_time, msg.time, active_notes[msg.note].velocity, sample_rate, 0);
                active_notes[msg.note].active = 0;
            }
        }
    }
    for (int i = 0; i < 128; i++) if (active_notes[i].active) render_note(output, num_samples, i, active_notes[i].start_time, duration, active_notes[i].velocity, sample_rate, 1);
    return output;
}

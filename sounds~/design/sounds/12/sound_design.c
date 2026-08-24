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
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} BiquadFilter;

static void setup_highpass(BiquadFilter* f, double freq, double Q, double sample_rate) {
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double b0 = (1.0 + cos(w0)) / 2.0;
    double b1 = -(1.0 + cos(w0));
    double b2 = (1.0 + cos(w0)) / 2.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cos(w0);
    double a2 = 1.0 - alpha;

    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0;
}

static double process_biquad(BiquadFilter* f, double in) {
    double out = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = in;
    f->y2 = f->y1;
    f->y1 = out;
    return out;
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
    double base_freq = 440.0 * pow(2.0, (note_num - 69) / 12.0);

    double attack_time = 0.002;
    double decay_time = 0.600;
    double sustain_level = 0.30;
    double release_time = 0.250;

    double note_duration = is_sustained ? (end_time - start_time) : (end_time - start_time + release_time);
    int note_samples = (int)(note_duration * sample_rate);
    int start_idx = (int)(start_time * sample_rate);
    int end_idx = start_idx + note_samples;
    if (end_idx > num_samples) end_idx = num_samples;
    int actual_samples = end_idx - start_idx;
    if (actual_samples <= 0) return;

    double* env = (double*)malloc(actual_samples * sizeof(double));
    adsr_envelope(env, actual_samples, (int)(attack_time * sample_rate), (int)(decay_time * sample_rate), sustain_level, (int)(release_time * sample_rate), is_sustained);

    // Highpass filter for mallet noise strike transient
    BiquadFilter mallet_hp;
    setup_highpass(&mallet_hp, 2500.0, 0.707, (double)sample_rate);

    // Stiff metal bar inharmonic mode frequency ratios
    double mode_ratios[5] = {1.0, 2.756, 5.404, 8.933, 13.344};
    double mode_weights[5] = {1.0, 0.60, 0.35, 0.20, 0.10};
    double mode_damp[5] = {1.0, 2.2, 4.5, 8.0, 14.0}; // higher partials decay exponentially faster
    double mode_phases[5] = {0.0, 0.0, 0.0, 0.0, 0.0};

    double mod_phase = 0.0;
    double mod_ratio = 3.571; // Inharmonic FM ratio

    double ring_mod_phase = 0.0;
    double kick_phase = 0.0;

    srand(note_num * 100);

    // Gain calibration variable
    double current_gain = 1.172394792222333;

    for (int i = 0; i < actual_samples; i++) {
        double t_local = (double)i / sample_rate;

        // 1. Mallet Sub-Bass Pitch Sweep (Kick thump transient)
        double kick_env = exp(-t_local / 0.025);
        double kick_freq = 40.0 + 160.0 * exp(-t_local / 0.008);
        kick_phase += 2.0 * M_PI * kick_freq / sample_rate;
        if (kick_phase > 2.0 * M_PI) kick_phase -= 2.0 * M_PI;
        double sub_thump = sin(kick_phase) * kick_env * 0.45;

        // 2. Mallet Click Noise Transient
        double noise_env = exp(-t_local / 0.005);
        double white_noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
        double mallet_click = process_biquad(&mallet_hp, white_noise) * noise_env * 0.35;

        // 3. FM Modulator (Decaying modulation index for transient metallic brightness burst)
        double mod_index = 3.5 * exp(-t_local / 0.06) + 0.15;
        mod_phase += 2.0 * M_PI * (base_freq * mod_ratio) / sample_rate;
        if (mod_phase > 2.0 * M_PI) mod_phase -= 2.0 * M_PI;
        double mod_val = sin(mod_phase) * mod_index;

        // 4. Metal Bar Inharmonic Partial Summation with FM Phase Modulation
        double bar_sum = 0.0;
        for (int m = 0; m < 5; m++) {
            double partial_freq = base_freq * mode_ratios[m];
            mode_phases[m] += 2.0 * M_PI * partial_freq / sample_rate;
            if (mode_phases[m] > 2.0 * M_PI) mode_phases[m] -= 2.0 * M_PI;

            double partial_env = exp(-t_local * mode_damp[m] * 2.5);
            double partial_sig = sin(mode_phases[m] + mod_val * (m == 0 ? 1.0 : 0.4));
            bar_sum += partial_sig * mode_weights[m] * partial_env;
        }

        // 5. Dynamic Ring Modulation Partial Sweep (sweeps upwards over time)
        double ring_mod_freq = base_freq * (1.5 + 4.0 * (1.0 - exp(-t_local / 0.3)));
        ring_mod_phase += 2.0 * M_PI * ring_mod_freq / sample_rate;
        if (ring_mod_phase > 2.0 * M_PI) ring_mod_phase -= 2.0 * M_PI;
        double ring_mod_sig = bar_sum * sin(ring_mod_phase) * 0.25 * exp(-t_local / 0.15);

        // Combine components
        double combined = sub_thump + mallet_click + bar_sum + ring_mod_sig;

        // Warm metallic acoustic body saturation
        double saturated = tanh(combined * 1.3) / 1.3;

        output[start_idx + i] += saturated * env[i] * (velocity / 127.0) * current_gain;
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

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

static void setup_bandpass(BiquadFilter* f, double freq, double Q, double sample_rate) {
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double b0 = alpha;
    double b1 = 0.0;
    double b2 = -alpha;
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

static void setup_peaking(BiquadFilter* f, double freq, double Q, double dbGain, double sample_rate) {
    double A = pow(10.0, dbGain / 40.0);
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cos(w0);
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cos(w0);
    double a2 = 1.0 - alpha / A;

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

    // Bowing attack/decay/sustain/release times
    double attack_time = 0.045;   // Gradual bow pressure build-up
    double decay_time = 0.080;
    double sustain_level = 0.85;
    double release_time = 0.120;  // Natural string vibration decay

    double note_duration = is_sustained ? (end_time - start_time) : (end_time - start_time + release_time);
    int note_samples = (int)(note_duration * sample_rate);
    int start_idx = (int)(start_time * sample_rate);
    int end_idx = start_idx + note_samples;
    if (end_idx > num_samples) end_idx = num_samples;
    int actual_samples = end_idx - start_idx;
    if (actual_samples <= 0) return;

    double* env = (double*)malloc(actual_samples * sizeof(double));
    adsr_envelope(env, actual_samples, (int)(attack_time * sample_rate), (int)(decay_time * sample_rate), sustain_level, (int)(release_time * sample_rate), is_sustained);

    // Setup Acoustic Wood Body Formant Filters (Cello/Violin Wood Body Resonances)
    #define NUM_BODY_FILTERS 4
    BiquadFilter body_filters[NUM_BODY_FILTERS];
    // Air cavity resonance, Main Wood resonance, Wood body upper resonance, Bridge brilliance
    double body_freqs[NUM_BODY_FILTERS] = {280.0, 550.0, 2400.0, 3500.0};
    double body_qs[NUM_BODY_FILTERS]    = {3.5,   4.0,   2.5,    3.0};
    double body_gains[NUM_BODY_FILTERS] = {6.0,   8.0,   4.0,    3.0}; // dB gain

    for (int b = 0; b < NUM_BODY_FILTERS; b++) {
        setup_peaking(&body_filters[b], body_freqs[b], body_qs[b], body_gains[b], (double)sample_rate);
    }

    // High pass filter for rosin attack noise
    BiquadFilter rosin_hp;
    setup_bandpass(&rosin_hp, 4000.0, 1.0, (double)sample_rate);

    // State variables for string & bow interaction
    double phase = 0.0;
    double vibrato_phase = 0.0;
    double vibrato_speed = 5.5; // 5.5 Hz natural vibrato rate
    double vibrato_depth = 0.008; // subtle pitch modulation
    double string_state = 0.0;   // low pass smoothing on friction drive

    srand(note_num);

    // Gain calibration variable
    double current_gain = 0.824911000353;

    for (int i = 0; i < actual_samples; i++) {
        double t_local = (double)i / sample_rate;

        // Vibrato onset delay: vibrato fades in after note start
        double vibrato_onset = 1.0 - exp(-t_local / 0.15);
        vibrato_phase += 2.0 * M_PI * vibrato_speed / sample_rate;
        if (vibrato_phase > 2.0 * M_PI) vibrato_phase -= 2.0 * M_PI;

        double pitch_mod = 1.0 + vibrato_depth * vibrato_onset * sin(vibrato_phase);
        double inst_freq = base_freq * pitch_mod;

        // Advance bow string phase
        phase += inst_freq / sample_rate;
        if (phase >= 1.0) phase -= 1.0;

        // Idealized Bowed String Excitation (Stick-Slip Helmholtz motion approximation)
        double raw_saw = 2.0 * phase - 1.0;
        // Non-linear bow friction curve rounding (stick-slip friction non-linearity)
        double bow_friction_drive = tanh(3.0 * raw_saw) - 0.2 * raw_saw;

        // Subtle string damping (smoothing low-pass)
        string_state = 0.75 * string_state + 0.25 * bow_friction_drive;

        // Bow Rosin Attack transient (friction noise burst on note contact)
        double rosin_val = 0.0;
        if (t_local < 0.05) {
            double rosin_env = exp(-t_local / 0.012);
            double white_noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
            rosin_val = process_biquad(&rosin_hp, white_noise) * rosin_env * 0.25;
        }

        // Continuous subtle bow scratch/friction texture
        double continuous_bow_scratch = ((double)rand() / RAND_MAX * 2.0 - 1.0) * 0.03 * env[i];

        // Combine string excitation, rosin attack, and bow noise
        double raw_bowed_signal = string_state + rosin_val + continuous_bow_scratch;

        // Pass raw bowed signal through wooden acoustic body formant filter chain
        double body_out = raw_bowed_signal;
        for (int b = 0; b < NUM_BODY_FILTERS; b++) {
            body_out += process_biquad(&body_filters[b], raw_bowed_signal);
        }

        // Normalize body resonances
        body_out /= (1.0 + NUM_BODY_FILTERS * 0.5);

        // Saturation / Warm Wood Compression
        double saturated = tanh(body_out * 1.2) / 1.2;

        // Final output with ADSR envelope, velocity scaling, and gain factor
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

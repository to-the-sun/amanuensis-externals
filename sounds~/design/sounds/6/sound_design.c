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

static void setup_bpf(Biquad* f, double freq, double bw, int sample_rate) {
    double omega = 2.0 * M_PI * freq / sample_rate;
    double sn = sin(omega);
    double cs = cos(omega);
    double Q = freq / bw;
    double alpha = sn / (2.0 * Q);

    double a0 = 1.0 + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0;
    f->b2 = -alpha / a0;
    f->a1 = -2.0 * cs / a0;
    f->a2 = (1.0 - alpha) / a0;

    f->x1 = f->x2 = f->y1 = f->y2 = 0;
}

static double process_biquad(Biquad* f, double x) {
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
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
    if (!is_sustained) {
        for (int i = 0; i < r_len && current < duration_samples; i++, current++) buffer[current] = sustain_level * (1.0 - (double)i / r_len);
    }
    while (current < duration_samples) buffer[current++] = is_sustained ? sustain_level : 0.0;
}

static void render_note(double* output, int num_samples, int note_num, double start_time, double end_time, int velocity, int sample_rate, int is_sustained) {
    double freq = 440.0 * pow(2.0, (note_num - 69) / 12.0);

    // Formant parameters for "A" vowel
    double formant_freqs[] = {730.0, 1090.0, 2440.0};
    double formant_bws[] = {80.0, 90.0, 120.0};
    double formant_amps[] = {1.0, 0.5, 0.2};

    Biquad filters[3];
    for (int i = 0; i < 3; i++) {
        setup_bpf(&filters[i], formant_freqs[i], formant_bws[i], sample_rate);
    }

    double amp_attack = 0.05;
    double amp_decay = 0.1;
    double amp_sustain = 0.7;
    double amp_release = 0.2;

    double note_duration = is_sustained ? (end_time - start_time) : (end_time - start_time + amp_release);
    int note_samples = (int)(note_duration * sample_rate);

    int start_idx = (int)(start_time * sample_rate);
    if (start_idx >= num_samples) return;

    int end_idx = start_idx + note_samples;
    if (end_idx > num_samples) end_idx = num_samples;
    int actual_samples = end_idx - start_idx;

    if (actual_samples <= 0) return;

    double* amp_env = (double*)malloc(actual_samples * sizeof(double));
    adsr_envelope(amp_env, actual_samples, (int)(amp_attack * sample_rate), (int)(amp_decay * sample_rate), amp_sustain, (int)(amp_release * sample_rate), is_sustained);

    // Vibrato parameters
    double vib_freq = 5.0;
    double vib_depth = 0.005;

    for (int i = 0; i < actual_samples; i++) {
        double t = (double)i / sample_rate + start_time;

        // Oscillator with vibrato
        double current_vib = 1.0 + vib_depth * sin(2.0 * M_PI * vib_freq * t);
        double phase = t * freq * current_vib;
        double saw = 2.0 * (phase - floor(phase + 0.5));

        // Parallel formant filters
        double out = 0;
        for (int j = 0; j < 3; j++) {
            out += process_biquad(&filters[j], saw) * formant_amps[j];
        }

        output[start_idx + i] += out * amp_env[i] * (velocity / 127.0) * 0.25;
    }

    free(amp_env);
}

double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out) {
    int num_samples = (int)(duration * sample_rate);
    *num_samples_out = num_samples;
    double* output = (double*)calloc(num_samples, sizeof(double));

    ActiveNote active_notes[128];
    for (int i = 0; i < 128; i++) active_notes[i].active = 0;

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

    for (int i = 0; i < 128; i++) {
        if (active_notes[i].active) {
            render_note(output, num_samples, i, active_notes[i].start_time, duration, active_notes[i].velocity, sample_rate, 1);
        }
    }

    // Final soft clipping to prevent harsh digital distortion
    for (int i = 0; i < num_samples; i++) {
        output[i] = tanh(output[i]);
    }

    return output;
}

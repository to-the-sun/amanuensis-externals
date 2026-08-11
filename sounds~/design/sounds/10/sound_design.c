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

    double attack_time = 0.005;
    double decay_time = 0.05;
    double sustain_level = 0.95;
    double release_time = 0.08;

    double note_duration = is_sustained ? (end_time - start_time) : (end_time - start_time + release_time);
    int note_samples = (int)(note_duration * sample_rate);
    int start_idx = (int)(start_time * sample_rate);
    int end_idx = start_idx + note_samples;
    if (end_idx > num_samples) end_idx = num_samples;
    int actual_samples = end_idx - start_idx;
    if (actual_samples <= 0) return;

    double* env = (double*)malloc(actual_samples * sizeof(double));
    adsr_envelope(env, actual_samples, (int)(attack_time * sample_rate), (int)(decay_time * sample_rate), sustain_level, (int)(release_time * sample_rate), is_sustained);

    // Hammond Drawbar Harmonics
    #define NUM_DRAWBARS 9
    double drawbar_ratios[NUM_DRAWBARS] = {0.5, 1.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
    double drawbar_weights[NUM_DRAWBARS] = {1.0, 0.8, 1.0, 0.9, 0.5, 0.7, 0.4, 0.3, 0.5};

    // Leslie rotary speaker speed
    double leslie_speed = 6.2; // Hz (fast tremolo)
    double tremolo_depth = 0.12;
    double vibrato_depth = 0.015; // phase modulation depth

    // Hammond percussion (decaying 3rd harmonic)
    double perc_ratio = 3.0;
    double perc_weight = 0.6;
    double perc_decay_const = 5.0; // decay time constant (s) -> e^(-5 * t)

    // Key click state
    double prev_noise = 0.0;
    double click_hp_state = 0.0;

    // Use deterministic seed for noise consistency
    srand(note_num);

    for (int i = 0; i < actual_samples; i++) {
        double t_local = (double)i / sample_rate;
        double t_global = start_time + t_local;

        // Drawbar mix
        double organ_mix = 0.0;
        double sum_weights = 0.0;

        for (int d = 0; d < NUM_DRAWBARS; d++) {
            double r = drawbar_ratios[d];
            double w = drawbar_weights[d];

            // Slightly offset Leslie modulation per drawbar to simulate space/depth (rotating horn vs drum)
            double drawbar_phase_offset = d * (M_PI / NUM_DRAWBARS);
            double d_lfo = sin(2.0 * M_PI * leslie_speed * t_global + drawbar_phase_offset);

            // Amplitude modulation (Leslie Tremolo)
            double amp_mod = 1.0 + tremolo_depth * d_lfo;

            // Phase modulation (Leslie Doppler/Vibrato)
            double phase_mod = vibrato_depth * d_lfo * r;

            // Generate Sine
            double sine_val = sin(2.0 * M_PI * freq * r * t_global + phase_mod);

            organ_mix += sine_val * w * amp_mod;
            sum_weights += w;
        }

        if (sum_weights > 0.0) {
            organ_mix /= sum_weights;
        }

        // Hammond Percussion on 3rd harmonic
        double perc_env = exp(-perc_decay_const * t_local);
        double perc_val = sin(2.0 * M_PI * freq * perc_ratio * t_global) * perc_env * perc_weight;

        // Key click (short burst of high-passed noise at the start of the note)
        double click_val = 0.0;
        if (t_local < 0.012) {
            double click_env = exp(-t_local / 0.003); // very fast decay
            double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
            // High-pass filter key click
            double hp_out = noise - prev_noise + 0.85 * click_hp_state;
            click_hp_state = hp_out;
            prev_noise = noise;
            click_val = hp_out * click_env * 0.15;
        }

        // Total Mix
        double raw_mix = organ_mix * 0.8 + perc_val * 0.2 + click_val;

        // Tube Overdrive/Distortion (Warm Saturation)
        double drive = 1.4;
        double saturated = tanh(raw_mix * drive) / drive;

        // Apply ADSR envelope and velocity
        double current_gain = 1.396026072183;
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

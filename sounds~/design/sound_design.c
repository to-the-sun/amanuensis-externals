#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    f->x2 = f->x1; f->x1 = in;
    f->y2 = f->y1; f->y1 = out;
    return out;
}

typedef struct {
    int note;
    int velocity;
    int sample_rate;
    double freq;
    double vel_scale;
    int is_note_on;

    double attack_time, decay_time, sustain_level, release_time;
    double env_level;
    int env_stage;
    double release_start_level;

    BiquadFilter mallet_hp;
    double mode_phases[5];
    double mod_phase;
    double ring_mod_phase;
    double kick_phase;
    double t_local;
} Sound12Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound12Voice* v = (Sound12Voice*)calloc(1, sizeof(Sound12Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.002; v->decay_time = 0.600; v->sustain_level = 0.30; v->release_time = 0.250;
    v->env_level = 0.0; v->env_stage = 0;

    setup_highpass(&v->mallet_hp, 2500.0, 0.707, (double)sample_rate);
    for (int m = 0; m < 5; m++) v->mode_phases[m] = 0.0;
    v->mod_phase = 0.0; v->ring_mod_phase = 0.0; v->kick_phase = 0.0; v->t_local = 0.0;
    srand(note * 100);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound12Voice* v = (Sound12Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) { v->env_stage = 3; v->release_start_level = v->env_level; }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound12Voice* v = (Sound12Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double mode_ratios[5] = {1.0, 2.756, 5.404, 8.933, 13.344};
    double mode_weights[5] = {1.0, 0.60, 0.35, 0.20, 0.10};
    double mode_damp[5] = {1.0, 2.2, 4.5, 8.0, 14.0};
    double mod_ratio = 3.571;
    double gain = 1.172394792222333;

    for (int i = 0; i < num_samples; i++) {
        if (v->env_stage == 0) {
            v->env_level += dt / v->attack_time;
            if (v->env_level >= 1.0) { v->env_level = 1.0; v->env_stage = 1; }
        } else if (v->env_stage == 1) {
            v->env_level -= dt * (1.0 - v->sustain_level) / v->decay_time;
            if (v->env_level <= v->sustain_level) { v->env_level = v->sustain_level; v->env_stage = 2; }
        } else if (v->env_stage == 2) {
            v->env_level = v->sustain_level;
            if (!v->is_note_on) { v->env_stage = 3; v->release_start_level = v->env_level; }
        } else if (v->env_stage == 3) {
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.30) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        double kick_env = exp(-v->t_local / 0.025);
        double kick_freq = 40.0 + 160.0 * exp(-v->t_local / 0.008);
        v->kick_phase += 2.0 * M_PI * kick_freq / v->sample_rate;
        if (v->kick_phase > 2.0 * M_PI) v->kick_phase -= 2.0 * M_PI;
        double sub_thump = sin(v->kick_phase) * kick_env * 0.45;

        double noise_env = exp(-v->t_local / 0.005);
        double white_noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
        double mallet_click = process_biquad(&v->mallet_hp, white_noise) * noise_env * 0.35;

        double mod_index = 3.5 * exp(-v->t_local / 0.06) + 0.15;
        v->mod_phase += 2.0 * M_PI * (v->freq * mod_ratio) / v->sample_rate;
        if (v->mod_phase > 2.0 * M_PI) v->mod_phase -= 2.0 * M_PI;
        double mod_val = sin(v->mod_phase) * mod_index;

        double bar_sum = 0.0;
        for (int m = 0; m < 5; m++) {
            double partial_freq = v->freq * mode_ratios[m];
            v->mode_phases[m] += 2.0 * M_PI * partial_freq / v->sample_rate;
            if (v->mode_phases[m] > 2.0 * M_PI) v->mode_phases[m] -= 2.0 * M_PI;

            double partial_env = exp(-v->t_local * mode_damp[m] * 2.5);
            double partial_sig = sin(v->mode_phases[m] + mod_val * (m == 0 ? 1.0 : 0.4));
            bar_sum += partial_sig * mode_weights[m] * partial_env;
        }

        double ring_mod_freq = v->freq * (1.5 + 4.0 * (1.0 - exp(-v->t_local / 0.3)));
        v->ring_mod_phase += 2.0 * M_PI * ring_mod_freq / v->sample_rate;
        if (v->ring_mod_phase > 2.0 * M_PI) v->ring_mod_phase -= 2.0 * M_PI;
        double ring_mod_sig = bar_sum * sin(v->ring_mod_phase) * 0.25 * exp(-v->t_local / 0.15);

        double combined = sub_thump + mallet_click + bar_sum + ring_mod_sig;
        double saturated = tanh(combined * 1.3) / 1.3;

        buffer[i] += saturated * v->env_level * v->vel_scale * gain;
        v->t_local += dt;
    }

    return (v->env_stage < 4);
}

void free_voice(void* voice_ptr) {
    if (voice_ptr) free(voice_ptr);
}

double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out) {
    int num_samples = (int)(duration * sample_rate);
    *num_samples_out = num_samples;
    double* output = (double*)calloc(num_samples, sizeof(double));
    void* active_voices[128] = {NULL};

    int block_size = 64;
    for (int start = 0; start < num_samples; start += block_size) {
        int count = block_size;
        if (start + count > num_samples) count = num_samples - start;
        double cur_time = (double)start / sample_rate;
        double end_time = (double)(start + count) / sample_rate;

        for (int m = 0; m < num_messages; m++) {
            if (midi_messages[m].time >= cur_time && midi_messages[m].time < end_time) {
                int note = midi_messages[m].note;
                if (strcmp(midi_messages[m].type, "note_on") == 0 && midi_messages[m].velocity > 0) {
                    if (active_voices[note]) free_voice(active_voices[note]);
                    active_voices[note] = create_voice(note, midi_messages[m].velocity, sample_rate);
                } else if (strcmp(midi_messages[m].type, "note_off") == 0 || (strcmp(midi_messages[m].type, "note_on") == 0 && midi_messages[m].velocity == 0)) {
                    if (active_voices[note]) note_off_voice(active_voices[note]);
                }
            }
        }

        for (int n = 0; n < 128; n++) {
            if (active_voices[n]) {
                int still_active = process_voice(active_voices[n], output + start, count);
                if (!still_active) { free_voice(active_voices[n]); active_voices[n] = NULL; }
            }
        }
    }

    for (int n = 0; n < 128; n++) if (active_voices[n]) free_voice(active_voices[n]);
    return output;
}

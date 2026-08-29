#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

    double t_local;
    double t_global;
    double prev_noise;
    double click_hp_state;
} Sound10Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound10Voice* v = (Sound10Voice*)calloc(1, sizeof(Sound10Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.005; v->decay_time = 0.05; v->sustain_level = 0.95; v->release_time = 0.08;
    v->env_level = 0.0; v->env_stage = 0;
    v->t_local = 0.0; v->t_global = 0.0;
    v->prev_noise = 0.0; v->click_hp_state = 0.0;
    srand(note);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound10Voice* v = (Sound10Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) { v->env_stage = 3; v->release_start_level = v->env_level; }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound10Voice* v = (Sound10Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    #define NUM_DRAWBARS 9
    double drawbar_ratios[NUM_DRAWBARS] = {0.5, 1.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
    double drawbar_weights[NUM_DRAWBARS] = {1.0, 0.8, 1.0, 0.9, 0.5, 0.7, 0.4, 0.3, 0.5};

    double leslie_speed = 6.2;
    double tremolo_depth = 0.12;
    double vibrato_depth = 0.015;

    double perc_ratio = 3.0;
    double perc_weight = 0.6;
    double perc_decay_const = 5.0;

    double gain = 1.396026072183;

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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.95) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        double organ_mix = 0.0;
        double sum_weights = 0.0;

        for (int d = 0; d < NUM_DRAWBARS; d++) {
            double r = drawbar_ratios[d];
            double w = drawbar_weights[d];

            double drawbar_phase_offset = d * (M_PI / NUM_DRAWBARS);
            double d_lfo = sin(2.0 * M_PI * leslie_speed * v->t_global + drawbar_phase_offset);

            double amp_mod = 1.0 + tremolo_depth * d_lfo;
            double phase_mod = vibrato_depth * d_lfo * r;

            double sine_val = sin(2.0 * M_PI * v->freq * r * v->t_global + phase_mod);
            organ_mix += sine_val * w * amp_mod;
            sum_weights += w;
        }

        if (sum_weights > 0.0) organ_mix /= sum_weights;

        double perc_env = exp(-perc_decay_const * v->t_local);
        double perc_val = sin(2.0 * M_PI * v->freq * perc_ratio * v->t_global) * perc_env * perc_weight;

        double click_val = 0.0;
        if (v->t_local < 0.012) {
            double click_env = exp(-v->t_local / 0.003);
            double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
            double hp_out = noise - v->prev_noise + 0.85 * v->click_hp_state;
            v->click_hp_state = hp_out;
            v->prev_noise = noise;
            click_val = hp_out * click_env * 0.15;
        }

        double raw_mix = organ_mix * 0.8 + perc_val * 0.2 + click_val;
        double drive = 1.4;
        double saturated = tanh(raw_mix * drive) / drive;

        buffer[i] += saturated * v->env_level * v->vel_scale * gain;
        v->t_local += dt;
        v->t_global += dt;
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

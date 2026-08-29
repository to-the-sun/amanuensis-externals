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

    // Amp Env
    double amp_attack, amp_decay, amp_sustain, amp_release;
    double amp_env_level;
    int amp_env_stage;
    double amp_release_start;

    // Filter Env
    double filt_attack, filt_decay, filt_sustain, filt_release;
    double filt_env_level;
    int filt_env_stage;
    double filt_release_start;

    // Filter state
    double v0, v1;
    double phase;
} Sound3Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound3Voice* v = (Sound3Voice*)calloc(1, sizeof(Sound3Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->amp_attack = 0.01; v->amp_decay = 0.1; v->amp_sustain = 0.4; v->amp_release = 0.2;
    v->filt_attack = 0.05; v->filt_decay = 0.2; v->filt_sustain = 0.1; v->filt_release = 0.2;

    v->amp_env_level = 0.0; v->amp_env_stage = 0;
    v->filt_env_level = 0.0; v->filt_env_stage = 0;
    v->v0 = v->v1 = 0.0;
    v->phase = 0.0;
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound3Voice* v = (Sound3Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->amp_env_stage < 3) { v->amp_env_stage = 3; v->amp_release_start = v->amp_env_level; }
    if (v->filt_env_stage < 3) { v->filt_env_stage = 3; v->filt_release_start = v->filt_env_level; }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound3Voice* v = (Sound3Voice*)voice_ptr;
    if (v->amp_env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double base_cutoff = 100.0, env_amount = 3000.0;
    double gain = 1.04515;

    for (int i = 0; i < num_samples; i++) {
        // Amp Env update
        if (v->amp_env_stage == 0) {
            v->amp_env_level += dt / v->amp_attack;
            if (v->amp_env_level >= 1.0) { v->amp_env_level = 1.0; v->amp_env_stage = 1; }
        } else if (v->amp_env_stage == 1) {
            v->amp_env_level -= dt * (1.0 - v->amp_sustain) / v->amp_decay;
            if (v->amp_env_level <= v->amp_sustain) { v->amp_env_level = v->amp_sustain; v->amp_env_stage = 2; }
        } else if (v->amp_env_stage == 2) {
            v->amp_env_level = v->amp_sustain;
            if (!v->is_note_on) { v->amp_env_stage = 3; v->amp_release_start = v->amp_env_level; }
        } else if (v->amp_env_stage == 3) {
            v->amp_env_level -= dt * (v->amp_release_start > 0 ? v->amp_release_start : 0.4) / v->amp_release;
            if (v->amp_env_level <= 0.0) { v->amp_env_level = 0.0; v->amp_env_stage = 4; }
        }

        // Filt Env update
        if (v->filt_env_stage == 0) {
            v->filt_env_level += dt / v->filt_attack;
            if (v->filt_env_level >= 1.0) { v->filt_env_level = 1.0; v->filt_env_stage = 1; }
        } else if (v->filt_env_stage == 1) {
            v->filt_env_level -= dt * (1.0 - v->filt_sustain) / v->filt_decay;
            if (v->filt_env_level <= v->filt_sustain) { v->filt_env_level = v->filt_sustain; v->filt_env_stage = 2; }
        } else if (v->filt_env_stage == 2) {
            v->filt_env_level = v->filt_sustain;
            if (!v->is_note_on) { v->filt_env_stage = 3; v->filt_release_start = v->filt_env_level; }
        } else if (v->filt_env_stage == 3) {
            v->filt_env_level -= dt * (v->filt_release_start > 0 ? v->filt_release_start : 0.1) / v->filt_release;
            if (v->filt_env_level <= 0.0) { v->filt_env_level = 0.0; v->filt_env_stage = 4; }
        }

        if (v->amp_env_stage == 4) break;

        double t = v->phase;
        double saw = 2.0 * (t * v->freq - floor(t * v->freq + 0.5));
        double cutoff = base_cutoff + env_amount * v->filt_env_level;
        if (cutoff > v->sample_rate * 0.45) cutoff = v->sample_rate * 0.45;
        double q = 1.0; double g = tan(M_PI * cutoff / v->sample_rate); double k = 1.0 / q;
        double a1 = 1.0 / (1.0 + g * (g + k));
        double v2 = (saw - v->v1 * (g + k) - v->v0) * a1;
        double v1_next = v->v1 + g * v2; double v0_next = v->v0 + g * v1_next;
        double lowpass = v0_next; v->v1 = v1_next; v->v0 = v0_next;

        buffer[i] += lowpass * v->amp_env_level * v->vel_scale * gain;
        v->phase += dt;
    }

    return (v->amp_env_stage < 4);
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

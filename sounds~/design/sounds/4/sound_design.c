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

    // Mod Env
    double mod_attack, mod_decay, mod_sustain, mod_release;
    double mod_env_level;
    int mod_env_stage;
    double mod_release_start;

    double phase;
} Sound4Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound4Voice* v = (Sound4Voice*)calloc(1, sizeof(Sound4Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->amp_attack = 0.002; v->amp_decay = 0.8; v->amp_sustain = 0.05; v->amp_release = 0.3;
    v->mod_attack = 0.001; v->mod_decay = 0.15; v->mod_sustain = 0.1; v->mod_release = 0.1;

    v->amp_env_level = 0.0; v->amp_env_stage = 0;
    v->mod_env_level = 0.0; v->mod_env_stage = 0;
    v->phase = 0.0;
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound4Voice* v = (Sound4Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->amp_env_stage < 3) { v->amp_env_stage = 3; v->amp_release_start = v->amp_env_level; }
    if (v->mod_env_stage < 3) { v->mod_env_stage = 3; v->mod_release_start = v->mod_env_level; }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound4Voice* v = (Sound4Voice*)voice_ptr;
    if (v->amp_env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double mod_ratio = 2.718;
    double mod_index_max = 8.0;
    double mod_index_min = 0.5;
    double gain = 0.851774;

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
            v->amp_env_level -= dt * (v->amp_release_start > 0 ? v->amp_release_start : 0.05) / v->amp_release;
            if (v->amp_env_level <= 0.0) { v->amp_env_level = 0.0; v->amp_env_stage = 4; }
        }

        // Mod Env update
        if (v->mod_env_stage == 0) {
            v->mod_env_level += dt / v->mod_attack;
            if (v->mod_env_level >= 1.0) { v->mod_env_level = 1.0; v->mod_env_stage = 1; }
        } else if (v->mod_env_stage == 1) {
            v->mod_env_level -= dt * (1.0 - v->mod_sustain) / v->mod_decay;
            if (v->mod_env_level <= v->mod_sustain) { v->mod_env_level = v->mod_sustain; v->mod_env_stage = 2; }
        } else if (v->mod_env_stage == 2) {
            v->mod_env_level = v->mod_sustain;
            if (!v->is_note_on) { v->mod_env_stage = 3; v->mod_release_start = v->mod_env_level; }
        } else if (v->mod_env_stage == 3) {
            v->mod_env_level -= dt * (v->mod_release_start > 0 ? v->mod_release_start : 0.1) / v->mod_release;
            if (v->mod_env_level <= 0.0) { v->mod_env_level = 0.0; v->mod_env_stage = 4; }
        }

        if (v->amp_env_stage == 4) break;

        double t = v->phase;
        double mod_freq = v->freq * mod_ratio;
        double current_mod_index = mod_index_min + (mod_index_max - mod_index_min) * v->mod_env_level;
        double modulator = current_mod_index * sin(2.0 * M_PI * mod_freq * t);
        double wave = sin(2.0 * M_PI * v->freq * t + modulator);

        buffer[i] += wave * v->amp_env_level * v->vel_scale * gain;
        v->phase += dt;
    }

    return (v->amp_env_stage < 4);
}

void free_voice(void* voice_ptr) {
    if (voice_ptr) free(voice_ptr);
}

#define MAX_RENDER_VOICES 32

typedef struct {
    void* voice_ptr;
    int note;
    int releasing;
} RenderVoiceSlot;

double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out) {
    int num_samples = (int)(duration * sample_rate);
    *num_samples_out = num_samples;
    double* output = (double*)calloc(num_samples, sizeof(double));
    RenderVoiceSlot voices[MAX_RENDER_VOICES];
    for (int i = 0; i < MAX_RENDER_VOICES; i++) {
        voices[i].voice_ptr = NULL;
        voices[i].note = -1;
        voices[i].releasing = 0;
    }

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
                    for (int i = 0; i < MAX_RENDER_VOICES; i++) {
                        if (voices[i].voice_ptr && voices[i].note == note && !voices[i].releasing) {
                            voices[i].releasing = 1;
                            note_off_voice(voices[i].voice_ptr);
                        }
                    }
                    void* new_v = create_voice(note, midi_messages[m].velocity, sample_rate);
                    if (new_v) {
                        int slot = -1;
                        for (int i = 0; i < MAX_RENDER_VOICES; i++) {
                            if (!voices[i].voice_ptr) { slot = i; break; }
                        }
                        if (slot == -1) {
                            for (int i = 0; i < MAX_RENDER_VOICES; i++) {
                                if (voices[i].releasing) { slot = i; break; }
                            }
                        }
                        if (slot == -1) slot = 0;
                        if (voices[slot].voice_ptr) free_voice(voices[slot].voice_ptr);
                        voices[slot].voice_ptr = new_v;
                        voices[slot].note = note;
                        voices[slot].releasing = 0;
                    }
                } else if (strcmp(midi_messages[m].type, "note_off") == 0 || (strcmp(midi_messages[m].type, "note_on") == 0 && midi_messages[m].velocity == 0)) {
                    for (int i = 0; i < MAX_RENDER_VOICES; i++) {
                        if (voices[i].voice_ptr && voices[i].note == note && !voices[i].releasing) {
                            voices[i].releasing = 1;
                            note_off_voice(voices[i].voice_ptr);
                            break;
                        }
                    }
                }
            }
        }

        for (int i = 0; i < MAX_RENDER_VOICES; i++) {
            if (voices[i].voice_ptr) {
                int still_active = process_voice(voices[i].voice_ptr, output + start, count);
                if (!still_active) {
                    free_voice(voices[i].voice_ptr);
                    voices[i].voice_ptr = NULL;
                    voices[i].note = -1;
                    voices[i].releasing = 0;
                }
            }
        }
    }

    for (int i = 0; i < MAX_RENDER_VOICES; i++) {
        if (voices[i].voice_ptr) free_voice(voices[i].voice_ptr);
    }

    return output;
}
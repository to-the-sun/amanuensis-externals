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

    double attack_time;
    double decay_time;
    double sustain_level;
    double release_time;

    double env_level;
    int env_stage; // 0=attack, 1=decay, 2=sustain, 3=release, 4=done
    double release_start_level;

    double phase;
} Sound2Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound2Voice* v = (Sound2Voice*)calloc(1, sizeof(Sound2Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.02;
    v->decay_time = 0.1;
    v->sustain_level = 0.4;
    v->release_time = 0.15;

    v->env_level = 0.0;
    v->env_stage = 0;
    v->phase = 0.0;
    srand(note);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound2Voice* v = (Sound2Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3;
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound2Voice* v = (Sound2Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double mod_ratio = 3.5;
    double mod_index = 5.0;
    double gain = 0.7375497;

    for (int i = 0; i < num_samples; i++) {
        if (v->env_stage == 0) {
            double rate = dt / v->attack_time;
            v->env_level += rate;
            if (v->env_level >= 1.0) { v->env_level = 1.0; v->env_stage = 1; }
        } else if (v->env_stage == 1) {
            double rate = dt * (1.0 - v->sustain_level) / v->decay_time;
            v->env_level -= rate;
            if (v->env_level <= v->sustain_level) { v->env_level = v->sustain_level; v->env_stage = 2; }
        } else if (v->env_stage == 2) {
            v->env_level = v->sustain_level;
            if (!v->is_note_on) { v->env_stage = 3; v->release_start_level = v->env_level; }
        } else if (v->env_stage == 3) {
            double rate = dt * (v->release_start_level > 0 ? v->release_start_level : 0.4) / v->release_time;
            v->env_level -= rate;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        double t = v->phase;
        double modulator = mod_index * sin(2.0 * M_PI * v->freq * mod_ratio * t);
        double wave = sin(2.0 * M_PI * v->freq * t + modulator);
        double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0) * 0.05;

        buffer[i] += (wave + noise) * v->env_level * v->vel_scale * gain;
        v->phase += dt;
    }

    return (v->env_stage < 4);
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
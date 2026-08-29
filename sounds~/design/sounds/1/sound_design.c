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

    // Envelope
    double attack_time;
    double decay_time;
    double sustain_level;
    double release_time;

    double env_level;
    int env_stage; // 0=attack, 1=decay, 2=sustain, 3=release, 4=done
    double release_start_level;

    // Phase
    double phase;
} Sound1Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound1Voice* v = (Sound1Voice*)calloc(1, sizeof(Sound1Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.05;
    v->decay_time = 0.2;
    v->sustain_level = 0.6;
    v->release_time = 0.3;

    v->env_level = 0.0;
    v->env_stage = 0; // attack
    v->phase = 0.0;
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound1Voice* v = (Sound1Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3; // release stage
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound1Voice* v = (Sound1Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double harmonics[4][2] = {{1.0, 1.0}, {2.0, 0.4}, {3.0, 0.2}, {4.0, 0.1}};
    double gain = 0.546062;

    for (int i = 0; i < num_samples; i++) {
        // Update envelope stage
        if (v->env_stage == 0) { // Attack
            double rate = dt / v->attack_time;
            v->env_level += rate;
            if (v->env_level >= 1.0) {
                v->env_level = 1.0;
                v->env_stage = 1; // Decay
            }
        } else if (v->env_stage == 1) { // Decay
            double rate = dt * (1.0 - v->sustain_level) / v->decay_time;
            v->env_level -= rate;
            if (v->env_level <= v->sustain_level) {
                v->env_level = v->sustain_level;
                v->env_stage = 2; // Sustain
            }
        } else if (v->env_stage == 2) { // Sustain
            v->env_level = v->sustain_level;
            if (!v->is_note_on) {
                v->env_stage = 3;
                v->release_start_level = v->env_level;
            }
        } else if (v->env_stage == 3) { // Release
            double rate = dt * (v->release_start_level > 0 ? v->release_start_level : 0.6) / v->release_time;
            v->env_level -= rate;
            if (v->env_level <= 0.0) {
                v->env_level = 0.0;
                v->env_stage = 4; // Done
            }
        }

        if (v->env_stage == 4) {
            break;
        }

        // Synthesize wave
        double wave = 0.0;
        for (int h = 0; h < 4; h++) {
            wave += harmonics[h][1] * sin(2.0 * M_PI * v->freq * harmonics[h][0] * v->phase);
        }
        buffer[i] += wave * v->env_level * v->vel_scale * gain;

        v->phase += dt;
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
                    if (active_voices[note]) {
                        free_voice(active_voices[note]);
                    }
                    active_voices[note] = create_voice(note, midi_messages[m].velocity, sample_rate);
                } else if (strcmp(midi_messages[m].type, "note_off") == 0 || (strcmp(midi_messages[m].type, "note_on") == 0 && midi_messages[m].velocity == 0)) {
                    if (active_voices[note]) {
                        note_off_voice(active_voices[note]);
                    }
                }
            }
        }

        for (int n = 0; n < 128; n++) {
            if (active_voices[n]) {
                int still_active = process_voice(active_voices[n], output + start, count);
                if (!still_active) {
                    free_voice(active_voices[n]);
                    active_voices[n] = NULL;
                }
            }
        }
    }

    for (int n = 0; n < 128; n++) {
        if (active_voices[n]) free_voice(active_voices[n]);
    }

    return output;
}

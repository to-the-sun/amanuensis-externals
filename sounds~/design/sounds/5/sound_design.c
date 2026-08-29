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

    double* delay_line;
    int L;
    int ptr;
    double prev_val;
    int active;
} Sound5Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound5Voice* v = (Sound5Voice*)calloc(1, sizeof(Sound5Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;
    v->active = 1;

    v->L = (int)(sample_rate / v->freq);
    if (v->L < 2) v->L = 2;

    v->delay_line = (double*)malloc(v->L * sizeof(double));
    srand(note);
    for (int i = 0; i < v->L; i++) {
        v->delay_line[i] = ((double)rand() / RAND_MAX) * 2.0 - 1.0;
    }
    v->ptr = 0;
    v->prev_val = 0.0;
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound5Voice* v = (Sound5Voice*)voice_ptr;
    v->is_note_on = 0;
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound5Voice* v = (Sound5Voice*)voice_ptr;
    if (!v->active) return 0;

    double feedback = 0.994;
    double gain = 1.01485;

    for (int i = 0; i < num_samples; i++) {
        double current_val = v->delay_line[v->ptr];
        double filtered = (current_val + v->prev_val) * 0.5;
        v->prev_val = current_val;

        v->delay_line[v->ptr] = filtered * feedback;
        v->ptr++;
        if (v->ptr >= v->L) v->ptr = 0;

        buffer[i] += filtered * v->vel_scale * gain;

        // Check if string energy has decayed to silence after note_off
        if (!v->is_note_on && fabs(filtered) < 1e-5) {
            v->active = 0;
            break;
        }
    }

    return v->active;
}

void free_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound5Voice* v = (Sound5Voice*)voice_ptr;
    if (v->delay_line) free(v->delay_line);
    free(v);
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

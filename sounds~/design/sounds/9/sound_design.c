#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double x1, x2, y1, y2;
    double b0, b1, b2, a1, a2;
} Biquad;

static void update_lpf(Biquad* f, double cutoff, double Q, int sample_rate) {
    double omega = 2.0 * M_PI * cutoff / sample_rate;
    double sn = sin(omega);
    double cs = cos(omega);
    double alpha = sn / (2.0 * Q);

    double a0 = 1.0 + alpha;
    f->b0 = ((1.0 - cs) / 2.0) / a0;
    f->b1 = (1.0 - cs) / a0;
    f->b2 = ((1.0 - cs) / 2.0) / a0;
    f->a1 = (-2.0 * cs) / a0;
    f->a2 = (1.0 - alpha) / a0;
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

    double phases[5];
    double sub_phase;
    Biquad filter;
    double t_local;
} Sound9Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound9Voice* v = (Sound9Voice*)calloc(1, sizeof(Sound9Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.05; v->decay_time = 0.3; v->sustain_level = 0.6; v->release_time = 0.5;
    v->env_level = 0.0; v->env_stage = 0;

    v->phases[0] = 0.0; v->phases[1] = 0.2; v->phases[2] = 0.4; v->phases[3] = 0.6; v->phases[4] = 0.8;
    v->sub_phase = 0.0;
    v->t_local = 0.0;
    srand(note);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound9Voice* v = (Sound9Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) { v->env_stage = 3; v->release_start_level = v->env_level; }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound9Voice* v = (Sound9Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double osc_amps[5] = {0.18, 0.22, 0.3, 0.22, 0.18};
    double osc_detunes[5] = {0.990, 0.995, 1.0, 1.005, 1.010};
    double gain = 1.001565103444;

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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.6) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        double mix = 0.0;
        for (int osc = 0; osc < 5; osc++) {
            v->phases[osc] += (v->freq * osc_detunes[osc]) / v->sample_rate;
            if (v->phases[osc] >= 1.0) v->phases[osc] -= 1.0;
            mix += (2.0 * v->phases[osc] - 1.0) * osc_amps[osc];
        }

        v->sub_phase += (v->freq * 0.5) / v->sample_rate;
        if (v->sub_phase >= 1.0) v->sub_phase -= 1.0;
        double sub_osc = sin(2.0 * M_PI * v->sub_phase);

        double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0) * 0.15;
        double raw_signal = mix * 0.7 + sub_osc * 0.2 + noise * 0.1;

        double cutoff_start = v->freq * 6.5;
        double cutoff_end = v->freq * 1.5;
        if (cutoff_start > v->sample_rate * 0.45) cutoff_start = v->sample_rate * 0.45;
        if (cutoff_end > v->sample_rate * 0.45) cutoff_end = v->sample_rate * 0.45;
        if (cutoff_end < 80.0) cutoff_end = 80.0;

        double current_cutoff = cutoff_end + (cutoff_start - cutoff_end) * exp(-v->t_local / 0.35);
        update_lpf(&v->filter, current_cutoff, 2.2, v->sample_rate);

        double filtered = raw_signal;
        double y = v->filter.b0 * filtered + v->filter.b1 * v->filter.x1 + v->filter.b2 * v->filter.x2 - v->filter.a1 * v->filter.y1 - v->filter.a2 * v->filter.y2;
        v->filter.x2 = v->filter.x1; v->filter.x1 = filtered;
        v->filter.y2 = v->filter.y1; v->filter.y1 = y;

        buffer[i] += y * v->env_level * v->vel_scale * gain;
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

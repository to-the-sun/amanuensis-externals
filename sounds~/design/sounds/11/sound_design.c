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

static void setup_bandpass(BiquadFilter* f, double freq, double Q, double sample_rate) {
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha;
    f->b0 = alpha / a0;
    f->b1 = 0.0;
    f->b2 = -alpha / a0;
    f->a1 = -2.0 * cos(w0) / a0;
    f->a2 = (1.0 - alpha) / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0;
}

static void setup_peaking(BiquadFilter* f, double freq, double Q, double dbGain, double sample_rate) {
    double A = pow(10.0, dbGain / 40.0);
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double a0 = 1.0 + alpha / A;
    f->b0 = (1.0 + alpha * A) / a0;
    f->b1 = (-2.0 * cos(w0)) / a0;
    f->b2 = (1.0 - alpha * A) / a0;
    f->a1 = (-2.0 * cos(w0)) / a0;
    f->a2 = (1.0 - alpha / A) / a0;
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

    #define NUM_BODY_FILTERS 4
    BiquadFilter body_filters[NUM_BODY_FILTERS];
    BiquadFilter rosin_hp;

    double phase;
    double vibrato_phase;
    double string_state;
    double t_local;
} Sound11Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound11Voice* v = (Sound11Voice*)calloc(1, sizeof(Sound11Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.045; v->decay_time = 0.080; v->sustain_level = 0.85; v->release_time = 0.120;
    v->env_level = 0.0; v->env_stage = 0;

    double body_freqs[4] = {280.0, 550.0, 2400.0, 3500.0};
    double body_qs[4]    = {3.5,   4.0,   2.5,    3.0};
    double body_gains[4] = {6.0,   8.0,   4.0,    3.0};

    for (int b = 0; b < 4; b++) {
        setup_peaking(&v->body_filters[b], body_freqs[b], body_qs[b], body_gains[b], (double)sample_rate);
    }
    setup_bandpass(&v->rosin_hp, 4000.0, 1.0, (double)sample_rate);

    v->phase = 0.0; v->vibrato_phase = 0.0; v->string_state = 0.0; v->t_local = 0.0;
    srand(note);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound11Voice* v = (Sound11Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) { v->env_stage = 3; v->release_start_level = v->env_level; }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound11Voice* v = (Sound11Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double vibrato_speed = 5.5;
    double vibrato_depth = 0.008;
    double gain = 0.824911000353;

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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.85) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        double vibrato_onset = 1.0 - exp(-v->t_local / 0.15);
        v->vibrato_phase += 2.0 * M_PI * vibrato_speed / v->sample_rate;
        if (v->vibrato_phase > 2.0 * M_PI) v->vibrato_phase -= 2.0 * M_PI;

        double pitch_mod = 1.0 + vibrato_depth * vibrato_onset * sin(v->vibrato_phase);
        double inst_freq = v->freq * pitch_mod;

        v->phase += inst_freq / v->sample_rate;
        if (v->phase >= 1.0) v->phase -= 1.0;

        double raw_saw = 2.0 * v->phase - 1.0;
        double bow_friction_drive = tanh(3.0 * raw_saw) - 0.2 * raw_saw;
        v->string_state = 0.75 * v->string_state + 0.25 * bow_friction_drive;

        double rosin_val = 0.0;
        if (v->t_local < 0.05) {
            double rosin_env = exp(-v->t_local / 0.012);
            double white_noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
            rosin_val = process_biquad(&v->rosin_hp, white_noise) * rosin_env * 0.25;
        }

        double continuous_bow_scratch = ((double)rand() / RAND_MAX * 2.0 - 1.0) * 0.03 * v->env_level;
        double raw_bowed_signal = v->string_state + rosin_val + continuous_bow_scratch;

        double body_out = raw_bowed_signal;
        for (int b = 0; b < 4; b++) {
            body_out += process_biquad(&v->body_filters[b], raw_bowed_signal);
        }
        body_out /= (1.0 + 4 * 0.5);

        double saturated = tanh(body_out * 1.2) / 1.2;
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

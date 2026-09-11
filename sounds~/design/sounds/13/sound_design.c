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
} Biquad;

static void setup_bandpass(Biquad* f, double freq, double Q, double sample_rate) {
    if (freq < 20.0) freq = 20.0;
    if (freq > sample_rate * 0.45) freq = sample_rate * 0.45;
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double b0 = alpha;
    double b1 = 0.0;
    double b2 = -alpha;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cos(w0);
    double a2 = 1.0 - alpha;

    f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0;
    f->a1 = a1 / a0; f->a2 = a2 / a0;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0;
}

static double process_biquad(Biquad* f, double in) {
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

    double phases[4];
    double ring_phase;
    double sub_phase;

    Biquad bp_filter1;
    Biquad bp_filter2;

    double t_local;
} Sound13Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound13Voice* v = (Sound13Voice*)calloc(1, sizeof(Sound13Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    v->attack_time = 0.003;
    v->decay_time = 0.350;
    v->sustain_level = 0.40;
    v->release_time = 0.300;
    v->env_level = 0.0;
    v->env_stage = 0;

    for (int i = 0; i < 4; i++) v->phases[i] = 0.0;
    v->ring_phase = 0.0;
    v->sub_phase = 0.0;

    setup_bandpass(&v->bp_filter1, v->freq * 2.236, 4.0, (double)sample_rate);
    setup_bandpass(&v->bp_filter2, v->freq * 5.854, 5.0, (double)sample_rate);

    v->t_local = 0.0;
    srand(note * 97 + velocity * 13);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound13Voice* v = (Sound13Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3;
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound13Voice* v = (Sound13Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double gain = 2.3062567;

    // Inharmonic golden ratio overtones
    double ratios[4] = {1.0, 2.2360679, 3.618034, 5.854102};
    double weights[4] = {0.55, 0.45, 0.30, 0.20};

    for (int i = 0; i < num_samples; i++) {
        // ADSR Envelope
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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.40) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        // Pitch chirp on attack
        double chirp = 1.0 + 1.2 * exp(-v->t_local / 0.010);
        double f_base = v->freq * chirp;

        // Sub phase
        v->sub_phase += 2.0 * M_PI * (f_base * 0.5) / v->sample_rate;
        if (v->sub_phase > 2.0 * M_PI) v->sub_phase -= 2.0 * M_PI;
        double sub_sine = sin(v->sub_phase) * 0.35;

        // Inharmonic Partials
        double partial_sum = 0.0;
        for (int p = 0; p < 4; p++) {
            v->phases[p] += 2.0 * M_PI * (f_base * ratios[p]) / v->sample_rate;
            if (v->phases[p] > 2.0 * M_PI) v->phases[p] -= 2.0 * M_PI;

            double p_env = exp(-v->t_local * (0.5 + 1.5 * p));
            partial_sum += sin(v->phases[p]) * weights[p] * p_env;
        }

        // Ring Modulator
        double ring_freq = f_base * 1.7320508; // sqrt(3)
        v->ring_phase += 2.0 * M_PI * ring_freq / v->sample_rate;
        if (v->ring_phase > 2.0 * M_PI) v->ring_phase -= 2.0 * M_PI;
        double ring_mod = partial_sum * sin(v->ring_phase) * 0.6;

        // Wavefolder
        double fold_depth = 4.0 * exp(-v->t_local / 0.08) + 1.5;
        double folded = sin((partial_sum + ring_mod) * fold_depth);

        // Dynamic Formant Sweep
        double sweep1 = f_base * (2.236 + 3.0 * exp(-v->t_local / 0.12));
        double sweep2 = f_base * (5.854 * (1.0 + 0.5 * sin(2.0 * M_PI * 6.0 * v->t_local)));
        setup_bandpass(&v->bp_filter1, sweep1, 4.0, (double)v->sample_rate);
        setup_bandpass(&v->bp_filter2, sweep2, 5.0, (double)v->sample_rate);

        double bp1_out = process_biquad(&v->bp_filter1, folded);
        double bp2_out = process_biquad(&v->bp_filter2, folded);

        // High frequency noise click
        double click_env = exp(-v->t_local / 0.004);
        double click = ((double)rand() / RAND_MAX * 2.0 - 1.0) * click_env * 0.40;

        double combined = sub_sine + bp1_out * 0.6 + bp2_out * 0.5 + click;
        double saturated = tanh(combined * 1.4) / 1.4;

        buffer[i] += saturated * v->env_level * v->vel_scale * gain;
        v->t_local += dt;
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

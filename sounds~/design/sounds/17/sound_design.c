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
    if (freq < 30.0) freq = 30.0;
    if (freq > sample_rate * 0.45) freq = sample_rate * 0.45;
    if (Q < 0.1) Q = 0.1;
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
    int env_stage; // 0=attack, 1=decay, 2=sustain, 3=release, 4=done
    double release_start_level;

    double carrier_phase;
    double mod_phase;
    double lfo_phase;
    double transient_phase;
    double t_local;

    double pitch_comp;

    BiquadFilter formant1;
    BiquadFilter formant2;
} Sound17Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound17Voice* v = (Sound17Voice*)calloc(1, sizeof(Sound17Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    // ADSR Envelope
    v->attack_time = 0.025;
    v->decay_time = 0.220;
    v->sustain_level = 0.50;
    v->release_time = 0.200;
    v->env_level = 0.0;
    v->env_stage = 0;

    v->carrier_phase = 0.0;
    v->mod_phase = 0.0;
    v->lfo_phase = 0.0;
    v->transient_phase = 0.0;
    v->t_local = 0.0;

    // Pitch compensation for balanced frequency amplitude across keybed
    v->pitch_comp = pow(261.63 / v->freq, 0.05);

    setup_bandpass(&v->formant1, 500.0, 3.0, (double)sample_rate);
    setup_bandpass(&v->formant2, 1800.0, 4.0, (double)sample_rate);

    srand(note * 179 + velocity * 83);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound17Voice* v = (Sound17Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3;
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound17Voice* v = (Sound17Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double gain = 15.049512;

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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.50) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        // LFO (5.2 Hz) for pitch vibrato
        v->lfo_phase += 2.0 * M_PI * 5.2 * dt;
        if (v->lfo_phase > 2.0 * M_PI) v->lfo_phase -= 2.0 * M_PI;
        double lfo_val = sin(v->lfo_phase);
        double pitch_mod = 1.0 + 0.006 * lfo_val;

        double cur_freq = v->freq * pitch_mod;

        // Inharmonic modulator phase (ratio = 2.41421356 = 1 + sqrt(2))
        v->mod_phase += 2.0 * M_PI * (cur_freq * 2.41421356) * dt;
        if (v->mod_phase > 2.0 * M_PI) v->mod_phase -= 2.0 * M_PI;

        double mod_idx = (1.2 + 1.8 * v->vel_scale) * exp(-v->t_local / 0.25);
        double mod_signal = sin(v->mod_phase) * mod_idx;

        // Carrier phase with phase modulation
        v->carrier_phase += 2.0 * M_PI * cur_freq * dt;
        if (v->carrier_phase > 2.0 * M_PI) v->carrier_phase -= 2.0 * M_PI;
        double carrier_val = sin(v->carrier_phase + mod_signal);

        // Attack transient chiff (burst of noise + chirp)
        double trans_env = exp(-v->t_local / 0.015);
        double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);
        v->transient_phase += 2.0 * M_PI * (cur_freq * 4.5) * dt;
        if (v->transient_phase > 2.0 * M_PI) v->transient_phase -= 2.0 * M_PI;
        double chiff = (0.6 * noise + 0.4 * sin(v->transient_phase)) * trans_env;

        // Dynamic formant filter frequencies
        double f1_center = 400.0 + 0.3 * cur_freq + 400.0 * (1.0 - exp(-v->t_local / 0.12));
        double f2_center = 1200.0 + 0.8 * cur_freq + 800.0 * sin(M_PI * v->env_level);

        setup_bandpass(&v->formant1, f1_center, 3.5, (double)v->sample_rate);
        setup_bandpass(&v->formant2, f2_center, 4.5, (double)v->sample_rate);

        double out1 = process_biquad(&v->formant1, carrier_val);
        double out2 = process_biquad(&v->formant2, carrier_val);

        double synthesized = (0.55 * out1 + 0.45 * out2 + 0.25 * chiff) * v->pitch_comp;

        buffer[i] += synthesized * v->env_level * v->vel_scale * gain;
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

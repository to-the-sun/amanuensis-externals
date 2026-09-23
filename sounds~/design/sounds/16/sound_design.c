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

static void setup_lowpass(BiquadFilter* f, double freq, double Q, double sample_rate) {
    if (freq < 20.0) freq = 20.0;
    if (freq > sample_rate * 0.45) freq = sample_rate * 0.45;
    double w0 = 2.0 * M_PI * freq / sample_rate;
    double alpha = sin(w0) / (2.0 * Q);
    double b0 = (1.0 - cos(w0)) / 2.0;
    double b1 = 1.0 - cos(w0);
    double b2 = (1.0 - cos(w0)) / 2.0;
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

    double main_phase;
    double sub1_phase;
    double sub2_phase;
    double ring_phase;
    double lfo_phase;
    double t_local;

    // Bitcrush / sample hold state
    double hold_sample;
    int hold_count;

    BiquadFilter lp_filter;
} Sound16Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound16Voice* v = (Sound16Voice*)calloc(1, sizeof(Sound16Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    // ADSR Envelope
    v->attack_time = 0.050;
    v->decay_time = 0.300;
    v->sustain_level = 0.50;
    v->release_time = 0.250;
    v->env_level = 0.0;
    v->env_stage = 0;

    v->main_phase = 0.0;
    v->sub1_phase = 0.0;
    v->sub2_phase = 0.0;
    v->ring_phase = 0.0;
    v->lfo_phase = 0.0;
    v->t_local = 0.0;

    v->hold_sample = 0.0;
    v->hold_count = 0;

    setup_lowpass(&v->lp_filter, v->freq * 3.0, 1.5, (double)sample_rate);

    srand(note * 313 + velocity * 53);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound16Voice* v = (Sound16Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3;
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound16Voice* v = (Sound16Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double gain = 68.5769443;

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

        // Vibrato and tremolo LFO (3.5 Hz)
        v->lfo_phase += 2.0 * M_PI * 3.5 * dt;
        if (v->lfo_phase > 2.0 * M_PI) v->lfo_phase -= 2.0 * M_PI;
        double lfo_val = sin(v->lfo_phase);
        double pitch_mod = 1.0 + 0.005 * lfo_val;
        double tremolo = 1.0 + 0.12 * lfo_val;

        double cur_freq = v->freq * pitch_mod;

        // Main oscillator (saw / triangle morph)
        v->main_phase += 2.0 * M_PI * cur_freq * dt;
        if (v->main_phase > 2.0 * M_PI) v->main_phase -= 2.0 * M_PI;
        double main_osc = sin(v->main_phase) + 0.3 * sin(v->main_phase * 2.0);

        // Sub-harmonic oscillator 1 (freq / 2.0)
        v->sub1_phase += 2.0 * M_PI * (cur_freq * 0.5) * dt;
        if (v->sub1_phase > 2.0 * M_PI) v->sub1_phase -= 2.0 * M_PI;
        double sub1 = sin(v->sub1_phase);

        // Inharmonic metallic ring mod (freq * sqrt(7) = 2.645751)
        v->ring_phase += 2.0 * M_PI * (cur_freq * 2.6457513) * dt;
        if (v->ring_phase > 2.0 * M_PI) v->ring_phase -= 2.0 * M_PI;
        double ring = sin(v->ring_phase) * main_osc;

        double raw_mix = main_osc * 0.5 + sub1 * 0.4 + ring * 0.35;

        // Dynamic bitcrush / downsampling (hold every 4-8 samples based on velocity)
        int hold_period = 4 + (int)(4.0 * (1.0 - v->vel_scale));
        v->hold_count++;
        if (v->hold_count >= hold_period) {
            v->hold_sample = raw_mix;
            v->hold_count = 0;
        }

        // Dynamic lowpass filter sweep (starts high, sweeps down)
        double lp_cutoff = cur_freq * (1.5 + 6.0 * exp(-v->t_local / 0.18));
        setup_lowpass(&v->lp_filter, lp_cutoff, 1.2, (double)v->sample_rate);
        double filtered = process_biquad(&v->lp_filter, v->hold_sample);

        double voice_out = filtered * tremolo;

        buffer[i] += voice_out * v->env_level * v->vel_scale * gain;
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

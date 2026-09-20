#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define COMB_DELAY_MAX 8192

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

    // Comb filter delay line
    double delay_buffer[COMB_DELAY_MAX];
    int write_pos;
    double delay_samples;

    // Exciter phases
    double exciter_phase1;
    double exciter_phase2;
    double lfo_phase;
    double ring_phase;

    BiquadFilter damp_lp;

    double t_local;
} Sound14Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound14Voice* v = (Sound14Voice*)calloc(1, sizeof(Sound14Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    // ADSR Envelope
    v->attack_time = 0.002;
    v->decay_time = 0.450;
    v->sustain_level = 0.35;
    v->release_time = 0.250;
    v->env_level = 0.0;
    v->env_stage = 0;

    v->delay_samples = (double)sample_rate / v->freq;
    if (v->delay_samples > COMB_DELAY_MAX - 4) v->delay_samples = COMB_DELAY_MAX - 4;
    if (v->delay_samples < 4.0) v->delay_samples = 4.0;
    v->write_pos = 0;

    setup_lowpass(&v->damp_lp, v->freq * 4.0, 0.707, (double)sample_rate);

    v->exciter_phase1 = 0.0;
    v->exciter_phase2 = 0.0;
    v->lfo_phase = 0.0;
    v->ring_phase = 0.0;
    v->t_local = 0.0;

    srand(note * 137 + velocity * 29);
    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound14Voice* v = (Sound14Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3;
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound14Voice* v = (Sound14Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double gain = 1.62893933;

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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.35) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        // LFO for pitch flutter & feedback modulation (8.0 Hz)
        v->lfo_phase += 2.0 * M_PI * 8.0 / v->sample_rate;
        if (v->lfo_phase > 2.0 * M_PI) v->lfo_phase -= 2.0 * M_PI;
        double lfo_val = sin(v->lfo_phase);

        // Exciter signal: sharp burst of noise + FM chirp at note onset (first 15 ms)
        double exciter = 0.0;
        if (v->t_local < 0.025) {
            double burst_env = exp(-v->t_local / 0.005);
            double noise = ((double)rand() / RAND_MAX * 2.0 - 1.0);

            // Inharmonic FM chirp in exciter
            v->exciter_phase1 += 2.0 * M_PI * (v->freq * (3.1415 + 10.0 * exp(-v->t_local / 0.003))) / v->sample_rate;
            if (v->exciter_phase1 > 2.0 * M_PI) v->exciter_phase1 -= 2.0 * M_PI;

            double chirp = sin(v->exciter_phase1);
            exciter = (noise * 0.7 + chirp * 0.5) * burst_env;
        }

        // Ring modulator in body (sqrt(5) ratio)
        v->ring_phase += 2.0 * M_PI * (v->freq * 2.236068) / v->sample_rate;
        if (v->ring_phase > 2.0 * M_PI) v->ring_phase -= 2.0 * M_PI;
        double ring = sin(v->ring_phase) * 0.15 * exp(-v->t_local / 0.10);

        // Read from delay buffer with fractional interpolation
        double cur_delay = v->delay_samples + lfo_val * 0.5;
        double read_pos_f = (double)v->write_pos - cur_delay;
        while (read_pos_f < 0.0) read_pos_f += COMB_DELAY_MAX;

        int r_idx1 = (int)read_pos_f;
        int r_idx2 = (r_idx1 + 1) % COMB_DELAY_MAX;
        double frac = read_pos_f - r_idx1;
        double delayed_sample = (1.0 - frac) * v->delay_buffer[r_idx1] + frac * v->delay_buffer[r_idx2];

        // Damping lowpass filter in feedback loop
        double damp_cutoff = v->freq * (3.0 + 8.0 * exp(-v->t_local / 0.15));
        setup_lowpass(&v->damp_lp, damp_cutoff, 0.707, (double)v->sample_rate);
        double filtered_feedback = process_biquad(&v->damp_lp, delayed_sample);

        // Comb feedback gain (close to 0.985 for long metallic body resonance)
        double feedback_gain = 0.982;
        double comb_in = exciter + ring + filtered_feedback * feedback_gain;

        // Wavefolder / distortion on delay feedback input
        comb_in = tanh(comb_in * 1.8) / 1.8;

        v->delay_buffer[v->write_pos] = comb_in;
        v->write_pos = (v->write_pos + 1) % COMB_DELAY_MAX;

        double voice_out = comb_in + delayed_sample * 0.5;

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

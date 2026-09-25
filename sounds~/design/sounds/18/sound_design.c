#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Cytomic State Variable Filter (SVF) struct and processing
typedef struct {
    double ic1eq, ic2eq;
    double a1, a2, a3, k;
} SVFilter;

static void update_svf(SVFilter* f, double cutoff, double Q, double sample_rate) {
    if (cutoff < 20.0) cutoff = 20.0;
    if (cutoff > sample_rate * 0.45) cutoff = sample_rate * 0.45;
    if (Q < 0.5) Q = 0.5;

    double g = tan(M_PI * cutoff / sample_rate);
    double k = 1.0 / Q;
    f->a1 = 1.0 / (1.0 + g * (g + k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
    f->k = k;
}

static void process_svf(SVFilter* f, double in, double* lp, double* bp, double* hp) {
    double v3 = in - f->ic2eq;
    double v1 = f->a1 * f->ic1eq + f->a2 * v3;
    double v2 = f->ic2eq + f->a2 * f->ic1eq + f->a3 * v3;
    f->ic1eq = 2.0 * v1 - f->ic1eq;
    f->ic2eq = 2.0 * v2 - f->ic2eq;

    if (lp) *lp = v2;
    if (bp) *bp = v1;
    if (hp) *hp = in - f->k * v1 - v2;
}

// Normalized Wavefolder utility
static double wavefold(double in, double fold_amount) {
    double drive = in * (1.0 + fold_amount * 2.0);
    double folded = sin(drive + 0.3 * sin(drive * 2.0));
    return folded / (1.0 + 0.3 * fold_amount);
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
    double sub_phase;
    double ring_phase;
    double lfo_phase;
    double t_local;

    double pitch_comp;

    SVFilter svf;
} Sound18Voice;

static double midi_to_hz_tuned(int midi_note, double a4_hz) {
    return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
}

void* create_voice(int note, int velocity, int sample_rate) {
    Sound18Voice* v = (Sound18Voice*)calloc(1, sizeof(Sound18Voice));
    if (!v) return NULL;
    v->note = note;
    v->velocity = velocity;
    v->sample_rate = sample_rate;
    v->freq = midi_to_hz_tuned(note, 440.0);
    v->vel_scale = (double)velocity / 127.0;
    v->is_note_on = 1;

    // Fast punchy ADSR Envelope
    v->attack_time = 0.012;
    v->decay_time = 0.180;
    v->sustain_level = 0.45;
    v->release_time = 0.150;
    v->env_level = 0.0;
    v->env_stage = 0;

    v->main_phase = 0.0;
    v->sub_phase = 0.0;
    v->ring_phase = 0.0;
    v->lfo_phase = 0.0;
    v->t_local = 0.0;

    // Subtle pitch compensation across keybed
    v->pitch_comp = pow(261.63 / v->freq, 0.01);

    update_svf(&v->svf, v->freq * 2.0, 1.2, (double)sample_rate);

    return v;
}

void note_off_voice(void* voice_ptr) {
    if (!voice_ptr) return;
    Sound18Voice* v = (Sound18Voice*)voice_ptr;
    v->is_note_on = 0;
    if (v->env_stage < 3) {
        v->env_stage = 3;
        v->release_start_level = v->env_level;
    }
}

int process_voice(void* voice_ptr, double* buffer, int num_samples) {
    if (!voice_ptr) return 0;
    Sound18Voice* v = (Sound18Voice*)voice_ptr;
    if (v->env_stage == 4) return 0;

    double dt = 1.0 / v->sample_rate;
    double gain = 1.56012;

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
            v->env_level -= dt * (v->release_start_level > 0 ? v->release_start_level : 0.45) / v->release_time;
            if (v->env_level <= 0.0) { v->env_level = 0.0; v->env_stage = 4; }
        }

        if (v->env_stage == 4) break;

        // LFO (6.5 Hz) for subtle pitch/timbre modulation
        v->lfo_phase += 2.0 * M_PI * 6.5 * dt;
        if (v->lfo_phase > 2.0 * M_PI) v->lfo_phase -= 2.0 * M_PI;
        double lfo_val = sin(v->lfo_phase);

        // Pitch envelope (slight pitch drop at attack)
        double pitch_env = 1.0 + 0.015 * exp(-v->t_local / 0.03);
        double cur_freq = v->freq * pitch_env * (1.0 + 0.003 * lfo_val);

        // Main Oscillator: Saw/Triangle hybrid
        v->main_phase += 2.0 * M_PI * cur_freq * dt;
        if (v->main_phase > 2.0 * M_PI) v->main_phase -= 2.0 * M_PI;
        double tri_osc = (2.0 / M_PI) * asin(sin(v->main_phase));
        double saw_osc = 1.0 - (2.0 * v->main_phase / (2.0 * M_PI));
        double main_osc = 0.6 * tri_osc + 0.4 * saw_osc;

        // Sub Oscillator (1 octave down)
        v->sub_phase += 2.0 * M_PI * (cur_freq * 0.5) * dt;
        if (v->sub_phase > 2.0 * M_PI) v->sub_phase -= 2.0 * M_PI;
        double sub_osc = sin(v->sub_phase);

        // Ring Modulator Osc (Inharmonic ratio 2.73205 = 1 + sqrt(3))
        v->ring_phase += 2.0 * M_PI * (cur_freq * 2.732051) * dt;
        if (v->ring_phase > 2.0 * M_PI) v->ring_phase -= 2.0 * M_PI;
        double ring_mod_signal = sin(v->ring_phase);

        // Combine oscillators with velocity-dependent ring modulation
        double osc_mix = main_osc + 0.3 * sub_osc;
        double ring_amount = 0.35 * exp(-v->t_local / 0.20);
        double combined = osc_mix * (1.0 - ring_amount) + (osc_mix * ring_mod_signal) * ring_amount;

        // Apply non-linear Wavefolder
        double fold_env = 1.0 + 1.2 * v->env_level;
        double folded = wavefold(combined, fold_env);

        // Transient impact / burst (metallic chirp)
        double transient_env = exp(-v->t_local / 0.008);
        double chirp = sin(2.0 * M_PI * cur_freq * 7.5 * v->t_local) * transient_env * 0.25;

        // Filter cutoff envelope morphing
        double cutoff_sweep = v->freq * (1.5 + 4.0 * v->env_level * v->vel_scale);
        double q_factor = 1.2;
        update_svf(&v->svf, cutoff_sweep, q_factor, (double)v->sample_rate);

        double lp_out = 0.0, bp_out = 0.0, hp_out = 0.0;
        process_svf(&v->svf, folded + chirp, &lp_out, &bp_out, &hp_out);

        // Blend lowpass and bandpass filter outputs
        double filter_mix = 0.75 * lp_out + 0.25 * bp_out;

        double synthesized = filter_mix * v->pitch_comp;

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

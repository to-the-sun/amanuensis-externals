#ifndef SOUND_DESIGN_H
#define SOUND_DESIGN_H

#include <stdint.h>

#define SOUND_DESIGN_VERSION 12

typedef struct {
    char type[16];
    int note;
    int velocity;
    double time;
} MidiMessage;

// Stateful Voice API for real-time polyphonic synthesis
void* create_voice(int note, int velocity, int sample_rate);
void note_off_voice(void* voice_ptr);
int process_voice(void* voice_ptr, double* buffer, int num_samples);
void free_voice(void* voice_ptr);

// Offline rendering interface
double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out);

#endif

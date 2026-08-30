#ifndef ANALYSIS_UTILS_H
#define ANALYSIS_UTILS_H

#include <json-c/json.h>
#include "sound_design.h"

typedef struct {
    const char* name;
    MidiMessage* sequence;
    int sequence_len;
    double duration;
    const char* save_wav_filename;
} ProbeConfig;

#define NUM_PROBES 14
extern ProbeConfig PROBE_CONFIGS[NUM_PROBES];

extern MidiMessage DEFAULT_MIDI_SEQUENCE[];
extern int DEFAULT_MIDI_SEQUENCE_LEN;

void save_wav(const char* filename, double* buffer, int num_samples, int sample_rate);
double calculate_probe_distance(struct json_object* probe_res1, struct json_object* probe_res2);
double calculate_distance(struct json_object* results1, struct json_object* results2);
struct json_object* analyze_audio(double* audio, int num_samples, int sr);

#endif

#ifndef ANALYSIS_UTILS_H
#define ANALYSIS_UTILS_H

#include <json-c/json.h>
#include "sound_design.h"

// Multi-Probe Diagnostic Suites
// Baseline defaults when unvaried: Pitch = 60, Velocity = 80, Duration = 1000ms (1.0s)

extern MidiMessage PROBE_LENGTH_50MS[];
extern int PROBE_LENGTH_50MS_LEN;

extern MidiMessage PROBE_LENGTH_250MS[];
extern int PROBE_LENGTH_250MS_LEN;

extern MidiMessage PROBE_LENGTH_1000MS[];
extern int PROBE_LENGTH_1000MS_LEN;

extern MidiMessage PROBE_LENGTH_3000MS[];
extern int PROBE_LENGTH_3000MS_LEN;

extern MidiMessage PROBE_VELOCITY_16[];
extern int PROBE_VELOCITY_16_LEN;

extern MidiMessage PROBE_VELOCITY_48[];
extern int PROBE_VELOCITY_48_LEN;

extern MidiMessage PROBE_VELOCITY_80[];
extern int PROBE_VELOCITY_80_LEN;

extern MidiMessage PROBE_VELOCITY_127[];
extern int PROBE_VELOCITY_127_LEN;

extern MidiMessage PROBE_PITCH_24[];
extern int PROBE_PITCH_24_LEN;

extern MidiMessage PROBE_PITCH_48[];
extern int PROBE_PITCH_48_LEN;

extern MidiMessage PROBE_PITCH_72[];
extern int PROBE_PITCH_72_LEN;

extern MidiMessage PROBE_PITCH_96[];
extern int PROBE_PITCH_96_LEN;

extern MidiMessage PROBE_PHRASING_STACCATO[];
extern int PROBE_PHRASING_STACCATO_LEN;

extern MidiMessage PROBE_PHRASING_LEGATO[];
extern int PROBE_PHRASING_LEGATO_LEN;

typedef struct {
    const char* name;
    MidiMessage* sequence;
    int length;
    double duration;
} ProbeSuiteEntry;

extern ProbeSuiteEntry ALL_PROBES[];
extern int NUM_ALL_PROBES;

void save_wav(const char* filename, double* buffer, int num_samples, int sample_rate);
double calculate_distance(struct json_object* results1, struct json_object* results2);
struct json_object* analyze_audio(double* audio, int num_samples, int sr);

#endif

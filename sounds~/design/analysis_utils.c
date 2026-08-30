#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sndfile.h>
#include <aubio/aubio.h>
#include "analysis_utils.h"

// Length Probes (Baseline: Pitch 60, Velocity 80)
MidiMessage PROBE_LENGTH_50MS[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 0.050}
};
int PROBE_LENGTH_50MS_LEN = 2;

MidiMessage PROBE_LENGTH_250MS[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 0.250}
};
int PROBE_LENGTH_250MS_LEN = 2;

MidiMessage PROBE_LENGTH_1000MS[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 1.000}
};
int PROBE_LENGTH_1000MS_LEN = 2;

MidiMessage PROBE_LENGTH_3000MS[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 3.000}
};
int PROBE_LENGTH_3000MS_LEN = 2;

// Velocity Probes (Baseline: Pitch 60, Duration 1000ms)
MidiMessage PROBE_VELOCITY_16[] = {
    {"note_on", 60, 16, 0.0},
    {"note_off", 60, 0, 1.000}
};
int PROBE_VELOCITY_16_LEN = 2;

MidiMessage PROBE_VELOCITY_48[] = {
    {"note_on", 60, 48, 0.0},
    {"note_off", 60, 0, 1.000}
};
int PROBE_VELOCITY_48_LEN = 2;

MidiMessage PROBE_VELOCITY_80[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 1.000}
};
int PROBE_VELOCITY_80_LEN = 2;

MidiMessage PROBE_VELOCITY_127[] = {
    {"note_on", 60, 127, 0.0},
    {"note_off", 60, 0, 1.000}
};
int PROBE_VELOCITY_127_LEN = 2;

// Pitch Probes (Baseline: Velocity 80, Duration 1000ms)
MidiMessage PROBE_PITCH_24[] = {
    {"note_on", 24, 80, 0.0},
    {"note_off", 24, 0, 1.000}
};
int PROBE_PITCH_24_LEN = 2;

MidiMessage PROBE_PITCH_48[] = {
    {"note_on", 48, 80, 0.0},
    {"note_off", 48, 0, 1.000}
};
int PROBE_PITCH_48_LEN = 2;

MidiMessage PROBE_PITCH_72[] = {
    {"note_on", 72, 80, 0.0},
    {"note_off", 72, 0, 1.000}
};
int PROBE_PITCH_72_LEN = 2;

MidiMessage PROBE_PITCH_96[] = {
    {"note_on", 96, 80, 0.0},
    {"note_off", 96, 0, 1.000}
};
int PROBE_PITCH_96_LEN = 2;

// Phrasing Probes (Standardized pitch & velocity mix)
MidiMessage PROBE_PHRASING_STACCATO[] = {
    {"note_on", 60, 80, 0.000}, {"note_off", 60, 0, 0.080},
    {"note_on", 64, 110, 0.150}, {"note_off", 64, 0, 0.230},
    {"note_on", 67, 48, 0.300}, {"note_off", 67, 0, 0.380},
    {"note_on", 72, 127, 0.450}, {"note_off", 72, 0, 0.530},
    {"note_on", 55, 64, 0.600}, {"note_off", 55, 0, 0.680},
    {"note_on", 79, 96, 0.750}, {"note_off", 79, 0, 0.830}
};
int PROBE_PHRASING_STACCATO_LEN = 12;

MidiMessage PROBE_PHRASING_LEGATO[] = {
    {"note_on", 60, 80, 0.000},
    {"note_on", 64, 110, 0.300}, {"note_off", 60, 0, 0.350},
    {"note_on", 67, 48, 0.600}, {"note_off", 64, 0, 0.650},
    {"note_on", 72, 127, 0.900}, {"note_off", 67, 0, 0.950},
    {"note_on", 55, 64, 1.200}, {"note_off", 72, 0, 1.250},
    {"note_on", 79, 96, 1.500}, {"note_off", 55, 0, 1.550},
    {"note_off", 79, 0, 2.000}
};
int PROBE_PHRASING_LEGATO_LEN = 12;

ProbeSuiteEntry ALL_PROBES[] = {
    {"length_50ms", PROBE_LENGTH_50MS, 2, 0.500},
    {"length_250ms", PROBE_LENGTH_250MS, 2, 0.750},
    {"length_1000ms", PROBE_LENGTH_1000MS, 2, 1.500},
    {"length_3000ms", PROBE_LENGTH_3000MS, 2, 3.500},
    {"velocity_16", PROBE_VELOCITY_16, 2, 1.500},
    {"velocity_48", PROBE_VELOCITY_48, 2, 1.500},
    {"velocity_80", PROBE_VELOCITY_80, 2, 1.500},
    {"velocity_127", PROBE_VELOCITY_127, 2, 1.500},
    {"pitch_24", PROBE_PITCH_24, 2, 1.500},
    {"pitch_48", PROBE_PITCH_48, 2, 1.500},
    {"pitch_72", PROBE_PITCH_72, 2, 1.500},
    {"pitch_96", PROBE_PITCH_96, 2, 1.500},
    {"phrasing_staccato", PROBE_PHRASING_STACCATO, 12, 1.500},
    {"phrasing_legato", PROBE_PHRASING_LEGATO, 12, 2.500}
};
int NUM_ALL_PROBES = sizeof(ALL_PROBES) / sizeof(ProbeSuiteEntry);

void save_wav(const char* filename, double* buffer, int num_samples, int sample_rate) {
    SF_INFO sfinfo;
    sfinfo.channels = 1;
    sfinfo.samplerate = sample_rate;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* outfile = sf_open(filename, SFM_WRITE, &sfinfo);
    if (!outfile) {
        fprintf(stderr, "Error: could not open output file %s\n", filename);
        return;
    }

    sf_write_double(outfile, buffer, num_samples);
    sf_close(outfile);
}

// Strategy A Active Region Segmentation distance calculation across probes
double calculate_distance(struct json_object* results1, struct json_object* results2) {
    struct json_object *probes1, *probes2;
    if (!json_object_object_get_ex(results1, "probe_results", &probes1) ||
        !json_object_object_get_ex(results2, "probe_results", &probes2)) {
        return -1.0;
    }

    double total_distance = 0.0;
    int probe_count = 0;

    for (int p = 0; p < NUM_ALL_PROBES; p++) {
        const char* probe_name = ALL_PROBES[p].name;
        struct json_object *p1, *p2;

        if (!json_object_object_get_ex(probes1, probe_name, &p1) ||
            !json_object_object_get_ex(probes2, probe_name, &p2)) {
            continue;
        }

        struct json_object *mfcc_obj1, *mfcc_obj2;
        struct json_object *active1_obj, *active2_obj;

        if (!json_object_object_get_ex(p1, "mfccs", &mfcc_obj1) ||
            !json_object_object_get_ex(p2, "mfccs", &mfcc_obj2) ||
            !json_object_object_get_ex(p1, "active_frames", &active1_obj) ||
            !json_object_object_get_ex(p2, "active_frames", &active2_obj)) {
            continue;
        }

        int active1 = json_object_get_int(active1_obj);
        int active2 = json_object_get_int(active2_obj);
        int len1 = json_object_array_length(mfcc_obj1);
        int len2 = json_object_array_length(mfcc_obj2);

        int max_active = active1 > active2 ? active1 : active2;
        if (max_active == 0) continue;

        double probe_sum_sq = 0.0;
        int evaluated_frames = 0;

        for (int i = 0; i < max_active; i++) {
            // Strategy A: Only evaluate when at least one sound is within its active region.
            // If both sounds have extended beyond their active regions (i > active1 && i > active2), do nothing.
            struct json_object *f1 = (i < len1) ? json_object_array_get_idx(mfcc_obj1, i) : NULL;
            struct json_object *f2 = (i < len2) ? json_object_array_get_idx(mfcc_obj2, i) : NULL;

            for (int k = 0; k < 13; k++) {
                double val1 = (i < active1 && f1) ? json_object_get_double(json_object_array_get_idx(f1, k)) : 0.0;
                double val2 = (i < active2 && f2) ? json_object_get_double(json_object_array_get_idx(f2, k)) : 0.0;
                probe_sum_sq += (val1 - val2) * (val1 - val2);
            }
            evaluated_frames++;
        }

        if (evaluated_frames > 0) {
            total_distance += sqrt(probe_sum_sq / evaluated_frames);
            probe_count++;
        }
    }

    return probe_count > 0 ? (total_distance / probe_count) : 0.0;
}

struct json_object* analyze_audio(double* audio, int num_samples, int sr) {
    uint_t hop_size = (uint_t)(sr * 0.050); // 50ms hop
    uint_t win_size = (uint_t)(sr * 0.050); // 50ms window

    fvec_t *in = new_fvec(hop_size);
    aubio_pvoc_t *pv = new_aubio_pvoc(win_size, hop_size);
    cvec_t *fftgrain = new_cvec(win_size);
    aubio_mfcc_t *mfcc = new_aubio_mfcc(win_size, 40, 13, sr);
    fvec_t *out_mfcc = new_fvec(13);

    struct json_object *results = json_object_new_object();
    struct json_object *mfccs_temporal = json_object_new_array();

    int num_frames = 0;
    int active_frames = 0;
    double rms_silence_threshold = 0.0001; // -80dB threshold for active region

    for (int i = 0; i < num_samples; i += hop_size) {
        int remaining = num_samples - i;
        int current_hop = remaining < hop_size ? remaining : hop_size;
        for (int j = 0; j < current_hop; j++) in->data[j] = (smpl_t)audio[i + j];
        for (int j = current_hop; j < hop_size; j++) in->data[j] = 0;

        double rms_val = aubio_level_lin(in);
        if (rms_val > rms_silence_threshold) {
            active_frames = num_frames + 1;
        }

        aubio_pvoc_do(pv, in, fftgrain);
        aubio_mfcc_do(mfcc, fftgrain, out_mfcc);

        struct json_object *mfcc_frame = json_object_new_array();
        for (int j = 0; j < 13; j++) {
            json_object_array_add(mfcc_frame, json_object_new_double(out_mfcc->data[j]));
        }
        json_object_array_add(mfccs_temporal, mfcc_frame);
        num_frames++;
    }

    json_object_object_add(results, "total_frames", json_object_new_int(num_frames));
    json_object_object_add(results, "active_frames", json_object_new_int(active_frames));
    json_object_object_add(results, "mfccs", mfccs_temporal);

    del_fvec(in); del_aubio_pvoc(pv); del_cvec(fftgrain);
    del_aubio_mfcc(mfcc); del_fvec(out_mfcc);

    return results;
}

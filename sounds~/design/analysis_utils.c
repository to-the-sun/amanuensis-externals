#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sndfile.h>
#include <aubio/aubio.h>
#include "analysis_utils.h"

static MidiMessage probe_length_50ms[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 0.05}
};

static MidiMessage probe_length_250ms[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 0.25}
};

static MidiMessage probe_length_1000ms[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 1.0}
};

static MidiMessage probe_length_3000ms[] = {
    {"note_on", 60, 80, 0.0},
    {"note_off", 60, 0, 3.0}
};

static MidiMessage probe_vel_16[] = {
    {"note_on", 60, 16, 0.0},
    {"note_off", 60, 0, 1.0}
};

static MidiMessage probe_vel_52[] = {
    {"note_on", 60, 52, 0.0},
    {"note_off", 60, 0, 1.0}
};

static MidiMessage probe_vel_96[] = {
    {"note_on", 60, 96, 0.0},
    {"note_off", 60, 0, 1.0}
};

static MidiMessage probe_vel_127[] = {
    {"note_on", 60, 127, 0.0},
    {"note_off", 60, 0, 1.0}
};

static MidiMessage probe_pitch_24[] = {
    {"note_on", 24, 80, 0.0},
    {"note_off", 24, 0, 1.0}
};

static MidiMessage probe_pitch_48[] = {
    {"note_on", 48, 80, 0.0},
    {"note_off", 48, 0, 1.0}
};

static MidiMessage probe_pitch_72[] = {
    {"note_on", 72, 80, 0.0},
    {"note_off", 72, 0, 1.0}
};

static MidiMessage probe_pitch_96[] = {
    {"note_on", 96, 80, 0.0},
    {"note_off", 96, 0, 1.0}
};

static MidiMessage probe_phrasing_staccato[] = {
    {"note_on", 60, 80, 0.0},  {"note_off", 60, 0, 0.08},
    {"note_on", 64, 96, 0.2},  {"note_off", 64, 0, 0.28},
    {"note_on", 67, 110, 0.4}, {"note_off", 67, 0, 0.48},
    {"note_on", 72, 127, 0.6}, {"note_off", 72, 0, 0.68},
    {"note_on", 67, 90, 0.8},  {"note_off", 67, 0, 0.88},
    {"note_on", 64, 75, 1.0},  {"note_off", 64, 0, 1.08},
    {"note_on", 60, 60, 1.2},  {"note_off", 60, 0, 1.28},
    {"note_on", 72, 100, 1.4}, {"note_off", 72, 0, 1.48},
    {"note_on", 67, 85, 1.6},  {"note_off", 67, 0, 1.68},
    {"note_on", 60, 70, 1.8},  {"note_off", 60, 0, 1.88}
};

static MidiMessage probe_phrasing_legato[] = {
    {"note_on", 60, 80, 0.0},  {"note_off", 60, 0, 1.0},
    {"note_on", 64, 96, 0.6},  {"note_off", 64, 0, 1.8},
    {"note_on", 67, 110, 1.4}, {"note_off", 67, 0, 2.6},
    {"note_on", 72, 127, 2.2}, {"note_off", 72, 0, 3.7},
    {"note_on", 60, 80, 3.2},  {"note_off", 60, 0, 4.5}
};

ProbeConfig PROBE_CONFIGS[NUM_PROBES] = {
    {"length_50ms", probe_length_50ms, 2, 1.5, NULL},
    {"length_250ms", probe_length_250ms, 2, 1.5, NULL},
    {"length_1000ms", probe_length_1000ms, 2, 2.5, NULL},
    {"length_3000ms", probe_length_3000ms, 2, 4.5, NULL},
    {"vel_16", probe_vel_16, 2, 2.5, NULL},
    {"vel_52", probe_vel_52, 2, 2.5, NULL},
    {"vel_96", probe_vel_96, 2, 2.5, NULL},
    {"vel_127", probe_vel_127, 2, 2.5, NULL},
    {"pitch_24", probe_pitch_24, 2, 2.5, NULL},
    {"pitch_48", probe_pitch_48, 2, 2.5, NULL},
    {"pitch_72", probe_pitch_72, 2, 2.5, NULL},
    {"pitch_96", probe_pitch_96, 2, 2.5, NULL},
    {"phrasing_staccato", probe_phrasing_staccato, 20, 3.0, "staccato.wav"},
    {"phrasing_legato", probe_phrasing_legato, 10, 5.0, "legato.wav"}
};

MidiMessage DEFAULT_MIDI_SEQUENCE[] = {
    {"note_on", 60, 127, 0.0},
    {"note_off", 60, 0,   0.5},
    {"note_on", 64, 127, 0.5},
    {"note_off", 64, 0,   1.0},
    {"note_on", 67, 127, 1.0},
    {"note_off", 67, 0,   1.5},
    {"note_on", 72, 127, 1.5},
    {"note_off", 72, 0,   3.0},
};
int DEFAULT_MIDI_SEQUENCE_LEN = 8;

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

    double* clamped = (double*)malloc(num_samples * sizeof(double));
    if (clamped) {
        for (int i = 0; i < num_samples; i++) {
            double val = buffer[i];
            if (val > 1.0) val = 1.0;
            else if (val < -1.0) val = -1.0;
            clamped[i] = val;
        }
        sf_write_double(outfile, clamped, num_samples);
        free(clamped);
    } else {
        sf_write_double(outfile, buffer, num_samples);
    }
    sf_close(outfile);
}

double calculate_probe_distance(struct json_object* probe_res1, struct json_object* probe_res2) {
    struct json_object *temp1, *temp2;
    if (json_object_object_get_ex(probe_res1, "temporal_data", &temp1)) probe_res1 = temp1;
    if (json_object_object_get_ex(probe_res2, "temporal_data", &temp2)) probe_res2 = temp2;

    struct json_object *mfccs1, *mfccs2, *rms1, *rms2;
    if (!json_object_object_get_ex(probe_res1, "mfccs", &mfccs1) ||
        !json_object_object_get_ex(probe_res2, "mfccs", &mfccs2) ||
        !json_object_object_get_ex(probe_res1, "rms", &rms1) ||
        !json_object_object_get_ex(probe_res2, "rms", &rms2)) {
        return -1.0;
    }

    int len1 = json_object_array_length(mfccs1);
    int len2 = json_object_array_length(mfccs2);
    int max_len = len1 > len2 ? len1 : len2;

    int compared_frames = 0;
    double total_frame_dist = 0.0;

    for (int i = 0; i < max_len; i++) {
        double r1 = (i < len1) ? json_object_get_double(json_object_array_get_idx(rms1, i)) : 0.0;
        double r2 = (i < len2) ? json_object_get_double(json_object_array_get_idx(rms2, i)) : 0.0;

        int active1 = (i < len1 && r1 > 0.0001);
        int active2 = (i < len2 && r2 > 0.0001);

        if (!active1 && !active2) {
            continue;
        }

        struct json_object *f1 = active1 ? json_object_array_get_idx(mfccs1, i) : NULL;
        struct json_object *f2 = active2 ? json_object_array_get_idx(mfccs2, i) : NULL;

        double frame_sum_sq = 0.0;
        for (int k = 0; k < 13; k++) {
            double v1 = (active1 && f1) ? json_object_get_double(json_object_array_get_idx(f1, k)) : 0.0;
            double v2 = (active2 && f2) ? json_object_get_double(json_object_array_get_idx(f2, k)) : 0.0;
            double diff = v1 - v2;
            frame_sum_sq += diff * diff;
        }

        total_frame_dist += sqrt(frame_sum_sq);
        compared_frames++;
    }

    if (compared_frames == 0) return 0.0;
    return total_frame_dist / compared_frames;
}

double calculate_distance(struct json_object* results1, struct json_object* results2) {
    struct json_object *probes1, *probes2;
    if (!json_object_object_get_ex(results1, "probes", &probes1)) probes1 = results1;
    if (!json_object_object_get_ex(results2, "probes", &probes2)) probes2 = results2;

    double sum_dist = 0.0;
    int count = 0;

    for (int p = 0; p < NUM_PROBES; p++) {
        const char* name = PROBE_CONFIGS[p].name;
        struct json_object *p1, *p2;
        if (json_object_object_get_ex(probes1, name, &p1) &&
            json_object_object_get_ex(probes2, name, &p2)) {
            double d = calculate_probe_distance(p1, p2);
            if (d >= 0.0) {
                sum_dist += d;
                count++;
            }
        }
    }

    if (count == 0) return 0.0;
    return sum_dist / count;
}

struct json_object* analyze_audio(double* audio, int num_samples, int sr) {
    uint_t hop_size = (uint_t)(sr * 0.050);
    uint_t win_size = (uint_t)(sr * 0.050);

    fvec_t *in = new_fvec(hop_size);
    aubio_pvoc_t *pv = new_aubio_pvoc(win_size, hop_size);
    cvec_t *fftgrain = new_cvec(win_size);
    aubio_mfcc_t *mfcc = new_aubio_mfcc(win_size, 40, 13, sr);
    fvec_t *out_mfcc = new_fvec(13);

    struct json_object *results = json_object_new_object();
    struct json_object *rms_arr = json_object_new_array();
    struct json_object *mfccs_arr = json_object_new_array();

    for (int i = 0; i < num_samples; i += hop_size) {
        int remaining = num_samples - i;
        int current_hop = remaining < hop_size ? remaining : hop_size;
        for (int j = 0; j < current_hop; j++) in->data[j] = (smpl_t)audio[i + j];
        for (int j = current_hop; j < hop_size; j++) in->data[j] = 0;

        double rms_val = aubio_level_lin(in);
        json_object_array_add(rms_arr, json_object_new_double(rms_val));

        aubio_pvoc_do(pv, in, fftgrain);
        aubio_mfcc_do(mfcc, fftgrain, out_mfcc);

        struct json_object *mfcc_frame = json_object_new_array();
        for (int j = 0; j < 13; j++) {
            json_object_array_add(mfcc_frame, json_object_new_double(out_mfcc->data[j]));
        }
        json_object_array_add(mfccs_arr, mfcc_frame);
    }

    json_object_object_add(results, "rms", rms_arr);
    json_object_object_add(results, "mfccs", mfccs_arr);

    del_fvec(in);
    del_aubio_pvoc(pv);
    del_cvec(fftgrain);
    del_aubio_mfcc(mfcc);
    del_fvec(out_mfcc);

    return results;
}

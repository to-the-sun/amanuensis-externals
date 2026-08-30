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

    sf_write_double(outfile, buffer, num_samples);
    sf_close(outfile);
}

double calculate_probe_distance(struct json_object* probe_res1, struct json_object* probe_res2) {
    struct json_object *temp1, *temp2;
    if (!json_object_object_get_ex(probe_res1, "temporal_data", &temp1)) temp1 = probe_res1;
    if (!json_object_object_get_ex(probe_res2, "temporal_data", &temp2)) temp2 = probe_res2;

    struct json_object *mfccs1, *mfccs2, *rms1, *rms2;
    if (!json_object_object_get_ex(temp1, "mfccs", &mfccs1) ||
        !json_object_object_get_ex(temp2, "mfccs", &mfccs2) ||
        !json_object_object_get_ex(temp1, "rms", &rms1) ||
        !json_object_object_get_ex(temp2, "rms", &rms2)) {
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
    aubio_specdesc_t *sd_centroid = new_aubio_specdesc("centroid", win_size);
    aubio_specdesc_t *sd_kurtosis = new_aubio_specdesc("kurtosis", win_size);
    aubio_specdesc_t *sd_spread = new_aubio_specdesc("spread", win_size);
    aubio_pvoc_t *pv = new_aubio_pvoc(win_size, hop_size);
    cvec_t *fftgrain = new_cvec(win_size);
    fvec_t *out_centroid = new_fvec(1);
    fvec_t *out_kurtosis = new_fvec(1);
    fvec_t *out_spread = new_fvec(1);
    aubio_mfcc_t *mfcc = new_aubio_mfcc(win_size, 40, 13, sr);
    fvec_t *out_mfcc = new_fvec(13);
    aubio_tempo_t *tempo_obj = new_aubio_tempo("default", win_size, hop_size, sr);
    fvec_t *tempo_out = new_fvec(2);

    struct json_object *results = json_object_new_object();
    struct json_object *temporal_data = json_object_new_object();
    struct json_object *times_arr = json_object_new_array();
    struct json_object *rms_arr = json_object_new_array();
    struct json_object *centroid_arr = json_object_new_array();
    struct json_object *bandwidth_arr = json_object_new_array();
    struct json_object *flatness_arr = json_object_new_array();
    struct json_object *zcr_arr = json_object_new_array();
    struct json_object *mfccs_temporal = json_object_new_array();

    double total_rms = 0, peak_rms = 0, peak_amp = 0;
    for (int i = 0; i < num_samples; i++) {
        double abs_val = fabs(audio[i]);
        if (abs_val > peak_amp) peak_amp = abs_val;
    }

    double total_centroid = 0, total_bandwidth = 0, total_flatness = 0, total_zcr = 0;
    double mfcc_means[13] = {0};
    int num_frames = 0;

    for (int i = 0; i < num_samples; i += hop_size) {
        int remaining = num_samples - i;
        int current_hop = remaining < hop_size ? remaining : hop_size;
        for (int j = 0; j < current_hop; j++) in->data[j] = (smpl_t)audio[i + j];
        for (int j = current_hop; j < hop_size; j++) in->data[j] = 0;

        double rms_val = aubio_level_lin(in);
        total_rms += rms_val;
        if (rms_val > peak_rms) peak_rms = rms_val;
        json_object_array_add(rms_arr, json_object_new_double(rms_val));

        aubio_pvoc_do(pv, in, fftgrain);
        aubio_specdesc_do(sd_centroid, fftgrain, out_centroid);
        aubio_specdesc_do(sd_kurtosis, fftgrain, out_kurtosis);
        aubio_specdesc_do(sd_spread, fftgrain, out_spread);

        double c_val = aubio_bintofreq(out_centroid->data[0], sr, win_size);
        double f_val = out_kurtosis->data[0];
        double s_val = out_spread->data[0];
        total_centroid += c_val;
        total_flatness += f_val;
        total_bandwidth += s_val;
        json_object_array_add(centroid_arr, json_object_new_double(c_val));
        json_object_array_add(flatness_arr, json_object_new_double(f_val));
        json_object_array_add(bandwidth_arr, json_object_new_double(s_val));

        double zcr_val = 0;
        for (int j = 1; j < current_hop; j++) {
            if ((in->data[j-1] < 0 && in->data[j] >= 0) || (in->data[j-1] >= 0 && in->data[j] < 0)) zcr_val++;
        }
        zcr_val /= current_hop;
        total_zcr += zcr_val;
        json_object_array_add(zcr_arr, json_object_new_double(zcr_val));

        aubio_mfcc_do(mfcc, fftgrain, out_mfcc);
        struct json_object *mfcc_frame = json_object_new_array();
        for (int j = 0; j < 13; j++) {
            mfcc_means[j] += out_mfcc->data[j];
            json_object_array_add(mfcc_frame, json_object_new_double(out_mfcc->data[j]));
        }
        json_object_array_add(mfccs_temporal, mfcc_frame);
        aubio_tempo_do(tempo_obj, in, tempo_out);
        json_object_array_add(times_arr, json_object_new_double((double)i / sr));
        num_frames++;
    }

    if (num_frames > 0) {
        json_object_object_add(results, "average_rms", json_object_new_double(total_rms / num_frames));
        json_object_object_add(results, "peak_rms", json_object_new_double(peak_rms));
        json_object_object_add(results, "peak_amplitude", json_object_new_double(peak_amp));
        json_object_object_add(results, "average_spectral_centroid", json_object_new_double(total_centroid / num_frames));
        json_object_object_add(results, "average_spectral_bandwidth", json_object_new_double(total_bandwidth / num_frames));
        json_object_object_add(results, "average_spectral_flatness", json_object_new_double(total_flatness / num_frames));
        json_object_object_add(results, "average_zero_crossing_rate", json_object_new_double(total_zcr / num_frames));

        struct json_object *mfcc_means_arr = json_object_new_array();
        for (int j = 0; j < 13; j++) json_object_array_add(mfcc_means_arr, json_object_new_double(mfcc_means[j] / num_frames));
        json_object_object_add(results, "mfcc_means", mfcc_means_arr);
        json_object_object_add(results, "estimated_tempo", json_object_new_double(aubio_tempo_get_bpm(tempo_obj)));
    } else {
        json_object_object_add(results, "average_rms", json_object_new_double(0.0));
        json_object_object_add(results, "peak_rms", json_object_new_double(0.0));
        json_object_object_add(results, "peak_amplitude", json_object_new_double(0.0));
        json_object_object_add(results, "average_spectral_centroid", json_object_new_double(0.0));
        json_object_object_add(results, "average_spectral_bandwidth", json_object_new_double(0.0));
        json_object_object_add(results, "average_spectral_flatness", json_object_new_double(0.0));
        json_object_object_add(results, "average_zero_crossing_rate", json_object_new_double(0.0));
        struct json_object *mfcc_means_arr = json_object_new_array();
        for (int j = 0; j < 13; j++) json_object_array_add(mfcc_means_arr, json_object_new_double(0.0));
        json_object_object_add(results, "mfcc_means", mfcc_means_arr);
        json_object_object_add(results, "estimated_tempo", json_object_new_double(0.0));
    }

    json_object_object_add(temporal_data, "times", times_arr);
    json_object_object_add(temporal_data, "rms", rms_arr);
    json_object_object_add(temporal_data, "spectral_centroid", centroid_arr);
    json_object_object_add(temporal_data, "spectral_bandwidth", bandwidth_arr);
    json_object_object_add(temporal_data, "spectral_flatness", flatness_arr);
    json_object_object_add(temporal_data, "zero_crossing_rate", zcr_arr);
    json_object_object_add(temporal_data, "mfccs", mfccs_temporal);
    json_object_object_add(results, "temporal_data", temporal_data);

    del_fvec(in); del_aubio_specdesc(sd_centroid); del_aubio_specdesc(sd_kurtosis); del_aubio_specdesc(sd_spread);
    del_aubio_pvoc(pv); del_cvec(fftgrain); del_fvec(out_centroid); del_fvec(out_kurtosis); del_fvec(out_spread);
    del_aubio_mfcc(mfcc); del_fvec(out_mfcc); del_aubio_tempo(tempo_obj); del_fvec(tempo_out);

    return results;
}

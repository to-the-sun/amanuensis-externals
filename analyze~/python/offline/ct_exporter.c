#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cumulative_transience.h"

#pragma pack(push, 1)
typedef struct {
    char magic[4];          // "CTBN"
    uint32_t version;       // 1
    int32_t sample_rate;
    int32_t num_frames;
    int32_t window_ms;
    double tolerance;
    double max_peak_value;
    double min_score_seen;
    double max_score_seen;
    int32_t total_peaks;
} CTBinHeader;
#pragma pack(pop)

static int write_bytes(FILE* f, const void* ptr, size_t size, size_t count) {
    if (count == 0) return 1;
    return (fwrite(ptr, size, count, f) == count) && !ferror(f);
}

int export_ctbin(const char* output_filepath, const float* y, int len, int sr, int window_ms) {
    if (!output_filepath || !y || len <= 0 || sr <= 0) {
        return 0;
    }

    if (window_ms > 15000) {
        window_ms = 15000;
    } else if (window_ms <= 0) {
        window_ms = 15000;
    }

    FullAnalysisResult res;
    int success = analyzer_batch_analyze(y, len, sr, window_ms, &res);
    if (!success) {
        return 0;
    }

    FILE* f = fopen(output_filepath, "wb");
    if (!f) {
        analyzer_free_analysis(&res);
        return 0;
    }

    int32_t total_peaks = 0;
    for (int b = 0; b < MAX_BANDS; b++) {
        total_peaks += res.bands[b].num_peaks;
    }

    CTBinHeader header;
    memcpy(header.magic, "CTBN", 4);
    header.version = 1;
    header.sample_rate = sr;
    header.num_frames = res.num_frames;
    header.window_ms = window_ms;
    header.tolerance = res.tolerance;
    header.max_peak_value = res.max_peak_value;
    header.min_score_seen = res.min_score_seen;
    header.max_score_seen = res.max_score_seen;
    header.total_peaks = total_peaks;

    if (!write_bytes(f, &header, sizeof(CTBinHeader), 1)) {
        fclose(f);
        analyzer_free_analysis(&res);
        return 0;
    }

    int num_frames = res.num_frames;
    if (num_frames > 0) {
        if (!write_bytes(f, res.times, sizeof(float), num_frames) ||
            !write_bytes(f, res.ratings, sizeof(double), num_frames) ||
            !write_bytes(f, res.highest_peaks_ms, sizeof(double), num_frames) ||
            !write_bytes(f, res.demarcation_lines, sizeof(double), num_frames) ||
            !write_bytes(f, res.rolling_global_flux_avg, sizeof(float), num_frames) ||
            !write_bytes(f, res.rolling_global_smoothing_avg, sizeof(float), num_frames)) {
            fclose(f);
            analyzer_free_analysis(&res);
            return 0;
        }
    }

    int snap_len = window_ms + 1;

    for (int b = 0; b < MAX_BANDS; b++) {
        int num_b_peaks = res.bands[b].num_peaks;
        for (int k = 0; k < num_b_peaks; k++) {
            PeakResult* pr = &res.bands[b].peaks[k];

            int32_t p_idx = pr->p_idx;
            int32_t band_idx = pr->band_idx;
            double time_s = pr->time;
            double peak_val = pr->peak_val;
            double total_score = pr->total_score;
            double detected_peak_val = pr->detected_peak_val;
            double thresh_val = pr->thresh_val;
            double left_min = pr->left_min;
            double right_min = pr->right_min;
            double prominence = pr->prominence;
            int32_t num_qualifiers = pr->num_qualifiers;

            if (!write_bytes(f, &p_idx, sizeof(int32_t), 1) ||
                !write_bytes(f, &band_idx, sizeof(int32_t), 1) ||
                !write_bytes(f, &time_s, sizeof(double), 1) ||
                !write_bytes(f, &peak_val, sizeof(double), 1) ||
                !write_bytes(f, &total_score, sizeof(double), 1) ||
                !write_bytes(f, &detected_peak_val, sizeof(double), 1) ||
                !write_bytes(f, &thresh_val, sizeof(double), 1) ||
                !write_bytes(f, &left_min, sizeof(double), 1) ||
                !write_bytes(f, &right_min, sizeof(double), 1) ||
                !write_bytes(f, &prominence, sizeof(double), 1) ||
                !write_bytes(f, &num_qualifiers, sizeof(int32_t), 1)) {
                fclose(f);
                analyzer_free_analysis(&res);
                return 0;
            }

            for (int q = 0; q < num_qualifiers; q++) {
                double q_ms = pr->qualifiers[q].ms;
                double q_val = pr->qualifiers[q].val;
                double q_orig_ms = pr->qualifiers[q].orig_ms;
                if (!write_bytes(f, &q_ms, sizeof(double), 1) ||
                    !write_bytes(f, &q_val, sizeof(double), 1) ||
                    !write_bytes(f, &q_orig_ms, sizeof(double), 1)) {
                    fclose(f);
                    analyzer_free_analysis(&res);
                    return 0;
                }
            }

            if (!write_bytes(f, pr->snapshot, sizeof(double), snap_len)) {
                fclose(f);
                analyzer_free_analysis(&res);
                return 0;
            }
        }
    }

    fclose(f);
    analyzer_free_analysis(&res);
    return 1;
}

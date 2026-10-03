#include "ct_exporter.h"
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

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

typedef struct {
    double time_s;
    double new_value_ms;
} HPChangeEvent;

typedef struct {
    double start_ms;
    double end_ms;
    double duration_ms;
    double rating;
    int num_segments;
    int segment_indices[512];
} PatternDef;

typedef struct {
    int segment_index;
    double start_ms;
    double end_ms;
    double rating;
    int peak_count;
    float waveform_min[300];
    float waveform_max[300];
} SegmentInfo;

static int write_bytes(FILE* f, const void* ptr, size_t size, size_t count) {
    if (count == 0) return 1;
    return (fwrite(ptr, size, count, f) == count) && !ferror(f);
}

static void base64_encode(const unsigned char* in, size_t in_len, char* out) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < in_len) {
        uint32_t oct_a = i < in_len ? in[i++] : 0;
        int has_b = (i < in_len);
        uint32_t oct_b = has_b ? in[i++] : 0;
        int has_c = (i < in_len);
        uint32_t oct_c = has_c ? in[i++] : 0;

        uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;

        out[j++] = b64[(triple >> 18) & 0x3F];
        out[j++] = b64[(triple >> 12) & 0x3F];
        out[j++] = has_b ? b64[(triple >> 6) & 0x3F] : '=';
        out[j++] = has_c ? b64[triple & 0x3F] : '=';
    }
    out[j] = '\0';
}

int export_ctbin(const char* output_filepath, const float* y, int len, int sr, int window_ms) {
    if (!output_filepath || !y || len <= 0 || sr <= 0) return 0;
    if (window_ms > 15000 || window_ms <= 0) window_ms = 15000;

    FullAnalysisResult res;
    int success = analyzer_batch_analyze(y, len, sr, window_ms, &res);
    if (!success) return 0;

    FILE* f = fopen(output_filepath, "wb");
    if (!f) {
        analyzer_free_analysis(&res);
        return 0;
    }

    int32_t total_peaks = 0;
    for (int b = 0; b < MAX_BANDS; b++) total_peaks += res.bands[b].num_peaks;

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
        write_bytes(f, res.times, sizeof(float), num_frames);
        write_bytes(f, res.ratings, sizeof(double), num_frames);
        write_bytes(f, res.highest_peaks_ms, sizeof(double), num_frames);
        write_bytes(f, res.demarcation_lines, sizeof(double), num_frames);
        write_bytes(f, res.rolling_global_flux_avg, sizeof(float), num_frames);
        write_bytes(f, res.rolling_global_smoothing_avg, sizeof(float), num_frames);
    }

    int snap_len = window_ms + 1;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int k = 0; k < res.bands[b].num_peaks; k++) {
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

            write_bytes(f, &p_idx, sizeof(int32_t), 1);
            write_bytes(f, &band_idx, sizeof(int32_t), 1);
            write_bytes(f, &time_s, sizeof(double), 1);
            write_bytes(f, &peak_val, sizeof(double), 1);
            write_bytes(f, &total_score, sizeof(double), 1);
            write_bytes(f, &detected_peak_val, sizeof(double), 1);
            write_bytes(f, &thresh_val, sizeof(double), 1);
            write_bytes(f, &left_min, sizeof(double), 1);
            write_bytes(f, &right_min, sizeof(double), 1);
            write_bytes(f, &prominence, sizeof(double), 1);
            write_bytes(f, &num_qualifiers, sizeof(int32_t), 1);

            for (int q = 0; q < num_qualifiers; q++) {
                double q_ms = pr->qualifiers[q].ms;
                double q_val = pr->qualifiers[q].val;
                double q_orig_ms = pr->qualifiers[q].orig_ms;
                write_bytes(f, &q_ms, sizeof(double), 1);
                write_bytes(f, &q_val, sizeof(double), 1);
                write_bytes(f, &q_orig_ms, sizeof(double), 1);
            }
            write_bytes(f, pr->snapshot, sizeof(double), snap_len);
        }
    }

    fclose(f);
    analyzer_free_analysis(&res);
    return 1;
}

int export_all_assets_and_html(
    const char* audio_filepath,
    const char* output_dir,
    const char* stem_name,
    const float* y,
    int len,
    int sr,
    int pass1_window_ms
) {
    (void)audio_filepath;
    if (!output_dir || !stem_name || !y || len <= 0 || sr <= 0) return 0;

    // Pass 1: Headless Analysis with 15000ms window to determine high points
    FullAnalysisResult res1;
    if (!analyzer_batch_analyze(y, len, sr, pass1_window_ms, &res1)) return 0;

    // Track high point durations and changes across frames
    int num_frames = res1.num_frames;
    double frame_dt_s = (num_frames > 1) ? (res1.times[1] - res1.times[0]) : 0.001;

    HPChangeEvent hp_changes[1024];
    int num_hp_changes = 0;
    double last_hp_val = -999.0;

    // High point duration map
    double unique_hp_vals[1024];
    double unique_hp_durs[1024];
    int num_unique_hps = 0;

    for (int i = 0; i < num_frames; i++) {
        double raw_hp = res1.highest_peaks_ms[i];
        if (raw_hp == -999.0) continue;
        double val_ms = fabs(raw_hp);

        // Accumulate duration
        int found_idx = -1;
        for (int u = 0; u < num_unique_hps; u++) {
            if (fabs(unique_hp_vals[u] - val_ms) < 1e-3) {
                found_idx = u;
                break;
            }
        }
        if (found_idx >= 0) {
            unique_hp_durs[found_idx] += frame_dt_s;
        } else if (num_unique_hps < 1024) {
            unique_hp_vals[num_unique_hps] = val_ms;
            unique_hp_durs[num_unique_hps] = frame_dt_s;
            num_unique_hps++;
        }

        // High point change events
        if (last_hp_val < 0.0 || fabs(val_ms - last_hp_val) > 1e-3) {
            if (num_hp_changes < 1024) {
                hp_changes[num_hp_changes].time_s = res1.times[i];
                hp_changes[num_hp_changes].new_value_ms = val_ms;
                num_hp_changes++;
            }
            last_hp_val = val_ms;
        }
    }

    double best_bar_length_ms = 1000.0;
    double max_dur = 0.0;
    for (int u = 0; u < num_unique_hps; u++) {
        if (unique_hp_durs[u] > max_dur) {
            max_dur = unique_hp_durs[u];
            best_bar_length_ms = unique_hp_vals[u];
        }
    }
    if (best_bar_length_ms <= 0.0) best_bar_length_ms = 1000.0;

    analyzer_free_analysis(&res1);

    // Pass 2: Re-analyze with window_ms = min(15000, best_bar_length * 2)
    int pass2_win_ms = (int)round(best_bar_length_ms * 2.0);
    if (pass2_win_ms > 15000) pass2_win_ms = 15000;
    if (pass2_win_ms <= 0) pass2_win_ms = 1000;

    FullAnalysisResult res2;
    if (!analyzer_batch_analyze(y, len, sr, pass2_win_ms, &res2)) return 0;

    // Export .ctbin binary file for Pass 2
    char ctbin_path[4096];
    snprintf(ctbin_path, sizeof(ctbin_path), "%s/%s.ctbin", output_dir, stem_name);
    export_ctbin(ctbin_path, y, len, sr, pass2_win_ms);

    // Segment transience grouping & scoring
    double total_duration_ms = ((double)len / (double)sr) * 1000.0;
    int num_segments = (int)ceil(total_duration_ms / best_bar_length_ms);
    if (num_segments < 1) num_segments = 1;

    SegmentInfo* segments = (SegmentInfo*)calloc(num_segments, sizeof(SegmentInfo));

    for (int seg_i = 0; seg_i < num_segments; seg_i++) {
        segments[seg_i].segment_index = seg_i;
        segments[seg_i].start_ms = seg_i * best_bar_length_ms;
        segments[seg_i].end_ms = (seg_i + 1) * best_bar_length_ms;
        if (segments[seg_i].end_ms > total_duration_ms) segments[seg_i].end_ms = total_duration_ms;

        // Downsample audio waveform min/max for segment
        int s_sample = (int)round((segments[seg_i].start_ms / 1000.0) * sr);
        int e_sample = (int)round((segments[seg_i].end_ms / 1000.0) * sr);
        if (s_sample < 0) s_sample = 0;
        if (e_sample > len) e_sample = len;
        int seg_len_samples = e_sample - s_sample;

        if (seg_len_samples > 0) {
            int step = seg_len_samples / 300;
            if (step < 1) step = 1;
            for (int p = 0; p < 300; p++) {
                int p_start = s_sample + p * step;
                int p_end = p_start + step;
                if (p_start >= e_sample) p_start = e_sample - 1;
                if (p_end > e_sample) p_end = e_sample;

                float min_v = y[p_start];
                float max_v = y[p_start];
                for (int s = p_start; s < p_end; s++) {
                    if (y[s] < min_v) min_v = y[s];
                    if (y[s] > max_v) max_v = y[s];
                }
                segments[seg_i].waveform_min[p] = min_v;
                segments[seg_i].waveform_max[p] = max_v;
            }
        }

        // Calculate average peak score in segment
        double score_sum = 0.0;
        int peak_cnt = 0;
        for (int b = 0; b < MAX_BANDS; b++) {
            for (int k = 0; k < res2.bands[b].num_peaks; k++) {
                double peak_ms = res2.bands[b].peaks[k].time * 1000.0;
                if (peak_ms >= segments[seg_i].start_ms && peak_ms < segments[seg_i].end_ms) {
                    score_sum += res2.bands[b].peaks[k].total_score;
                    peak_cnt++;
                }
            }
        }
        segments[seg_i].peak_count = peak_cnt;
        segments[seg_i].rating = (peak_cnt > 0) ? (score_sum / peak_cnt) : 0.0;
    }

    // Pattern identification logic
    PatternDef patterns[256];
    int num_patterns = 0;

    if (num_segments >= 2) {
        int pat_start = 0;
        for (int seg_i = 1; seg_i < num_segments; seg_i++) {
            int pat_len = seg_i - pat_start;
            double min_rating_without = segments[pat_start].rating;
            for (int k = pat_start; k < seg_i; k++) {
                if (segments[k].rating < min_rating_without) min_rating_without = segments[k].rating;
            }
            double rating_without = min_rating_without * pat_len;

            double min_rating_with = min_rating_without;
            if (segments[seg_i].rating < min_rating_with) min_rating_with = segments[seg_i].rating;
            double rating_with = min_rating_with * (pat_len + 1);

            double standalone_rating = segments[seg_i].rating;

            if (rating_with <= rating_without || standalone_rating > rating_with) {
                if (pat_len >= 2 && num_patterns < 256) {
                    patterns[num_patterns].start_ms = segments[pat_start].start_ms;
                    patterns[num_patterns].end_ms = segments[seg_i - 1].end_ms;
                    patterns[num_patterns].duration_ms = patterns[num_patterns].end_ms - patterns[num_patterns].start_ms;
                    patterns[num_patterns].rating = rating_without;
                    patterns[num_patterns].num_segments = pat_len;
                    int num_segs_to_copy = pat_len < 512 ? pat_len : 512;
                    for (int k = 0; k < num_segs_to_copy; k++) patterns[num_patterns].segment_indices[k] = pat_start + k;
                    num_patterns++;
                }
                pat_start = seg_i;
            }
        }

        // Last pattern candidate
        int pat_len = num_segments - pat_start;
        if (pat_len >= 2 && num_patterns < 256) {
            double min_rating = segments[pat_start].rating;
            for (int k = pat_start; k < num_segments; k++) {
                if (segments[k].rating < min_rating) min_rating = segments[k].rating;
            }
            patterns[num_patterns].start_ms = segments[pat_start].start_ms;
            patterns[num_patterns].end_ms = segments[num_segments - 1].end_ms;
            patterns[num_patterns].duration_ms = patterns[num_patterns].end_ms - patterns[num_patterns].start_ms;
            patterns[num_patterns].rating = min_rating * pat_len;
            patterns[num_patterns].num_segments = pat_len;
            int num_segs_to_copy = pat_len < 512 ? pat_len : 512;
            for (int k = 0; k < num_segs_to_copy; k++) patterns[num_patterns].segment_indices[k] = pat_start + k;
            num_patterns++;
        }
    }

    // Cut & Save identified pattern WAV files directly using dr_wav
    for (int p = 0; p < num_patterns; p++) {
        char pat_wav_path[4096];
        snprintf(pat_wav_path, sizeof(pat_wav_path), "%s/pattern_%d.wav", output_dir, p + 1);

        uint64_t start_smp = (uint64_t)round((patterns[p].start_ms / 1000.0) * sr);
        uint64_t end_smp = (uint64_t)round((patterns[p].end_ms / 1000.0) * sr);
        if (start_smp >= (uint64_t)len) start_smp = len - 1;
        if (end_smp > (uint64_t)len) end_smp = len;
        uint64_t frame_count = (end_smp > start_smp) ? (end_smp - start_smp) : 0;

        drwav_data_format format;
        format.container = drwav_container_riff;
        format.format = DR_WAVE_FORMAT_PCM;
        format.channels = 1;
        format.sampleRate = sr;
        format.bitsPerSample = 16;

        drwav wav_out;
        if (drwav_init_file_write(&wav_out, pat_wav_path, &format, NULL)) {
            int16_t* pcm16 = (int16_t*)malloc(frame_count * sizeof(int16_t));
            if (pcm16) {
                for (uint64_t i = 0; i < frame_count; i++) {
                    float smp = y[start_smp + i];
                    if (smp > 1.0f) smp = 1.0f;
                    if (smp < -1.0f) smp = -1.0f;
                    pcm16[i] = (int16_t)(smp * 32767.0f);
                }
                drwav_write_pcm_frames(&wav_out, frame_count, pcm16);
                free(pcm16);
            }
            drwav_uninit(&wav_out);
        }
    }

    // Export snapshots.bin and snapshots.js
    int total_peaks = 0;
    for (int b = 0; b < MAX_BANDS; b++) total_peaks += res2.bands[b].num_peaks;

    char snap_bin_path[4096];
    snprintf(snap_bin_path, sizeof(snap_bin_path), "%s/snapshots.bin", output_dir);
    FILE* f_snap = fopen(snap_bin_path, "wb");

    int snap_len = pass2_win_ms + 1;
    size_t float32_bytes_per_snap = snap_len * sizeof(float);
    size_t total_snap_bytes = total_peaks * float32_bytes_per_snap;
    uint8_t* all_snap_buf = (uint8_t*)malloc(total_snap_bytes ? total_snap_bytes : 1);

    size_t snap_offset = 0;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int k = 0; k < res2.bands[b].num_peaks; k++) {
            PeakResult* pr = &res2.bands[b].peaks[k];
            float* float32_snap = (float*)malloc(snap_len * sizeof(float));
            for (int i = 0; i < snap_len; i++) float32_snap[i] = (float)pr->snapshot[i];

            if (f_snap) fwrite(float32_snap, sizeof(float), snap_len, f_snap);
            if (all_snap_buf) memcpy(all_snap_buf + snap_offset, float32_snap, float32_bytes_per_snap);

            snap_offset += float32_bytes_per_snap;
            free(float32_snap);
        }
    }
    if (f_snap) fclose(f_snap);

    char snap_js_path[4096];
    snprintf(snap_js_path, sizeof(snap_js_path), "%s/snapshots.js", output_dir);
    FILE* f_snap_js = fopen(snap_js_path, "w");
    if (f_snap_js && all_snap_buf) {
        size_t b64_len = 4 * ((total_snap_bytes + 2) / 3);
        char* b64_str = (char*)malloc(b64_len + 1);
        if (b64_str) {
            base64_encode(all_snap_buf, total_snap_bytes, b64_str);
            fprintf(f_snap_js, "window.snapshotsBase64 = '%s';\n", b64_str);
            free(b64_str);
        }
        fclose(f_snap_js);
    }
    free(all_snap_buf);

    // Export manifest.json
    char manifest_path[4096];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", output_dir);
    FILE* f_mf = fopen(manifest_path, "w");
    if (f_mf) {
        fprintf(f_mf, "{\n");
        fprintf(f_mf, "  \"version\": 1,\n");
        fprintf(f_mf, "  \"sample_rate\": %d,\n", sr);
        fprintf(f_mf, "  \"num_frames\": %d,\n", res2.num_frames);
        fprintf(f_mf, "  \"window_ms\": %d,\n", pass2_win_ms);
        fprintf(f_mf, "  \"tolerance\": %.4f,\n", res2.tolerance);
        fprintf(f_mf, "  \"max_peak_value\": %.6f,\n", res2.max_peak_value);
        fprintf(f_mf, "  \"min_score_seen\": %.6f,\n", res2.min_score_seen);
        fprintf(f_mf, "  \"max_score_seen\": %.6f,\n", res2.max_score_seen);
        fprintf(f_mf, "  \"total_peaks\": %d,\n", total_peaks);

        fprintf(f_mf, "  \"peaks\": [\n");
        int peak_iter = 0;
        size_t curr_snap_offset = 0;
        for (int b = 0; b < MAX_BANDS; b++) {
            for (int k = 0; k < res2.bands[b].num_peaks; k++) {
                PeakResult* pr = &res2.bands[b].peaks[k];
                fprintf(f_mf, "    {\n");
                fprintf(f_mf, "      \"p_idx\": %d,\n", pr->p_idx);
                fprintf(f_mf, "      \"band_idx\": %d,\n", pr->band_idx);
                fprintf(f_mf, "      \"time_s\": %.6f,\n", pr->time);
                fprintf(f_mf, "      \"time_ms\": %.3f,\n", pr->time * 1000.0);
                fprintf(f_mf, "      \"peak_val\": %.6f,\n", pr->peak_val);
                fprintf(f_mf, "      \"total_score\": %.6f,\n", pr->total_score);
                fprintf(f_mf, "      \"detected_peak_val\": %.6f,\n", pr->detected_peak_val);
                fprintf(f_mf, "      \"thresh_val\": %.6f,\n", pr->thresh_val);
                fprintf(f_mf, "      \"left_min\": %.6f,\n", pr->left_min);
                fprintf(f_mf, "      \"right_min\": %.6f,\n", pr->right_min);
                fprintf(f_mf, "      \"prominence\": %.6f,\n", pr->prominence);
                fprintf(f_mf, "      \"snap_offset\": %llu,\n", (unsigned long long)curr_snap_offset);
                fprintf(f_mf, "      \"snap_len\": %d,\n", snap_len);

                fprintf(f_mf, "      \"qualifiers\": [\n");
                for (int q = 0; q < pr->num_qualifiers; q++) {
                    fprintf(f_mf, "        {\"ms\": %.3f, \"val\": %.6f, \"orig_ms\": %.3f}%s\n",
                            pr->qualifiers[q].ms, pr->qualifiers[q].val, pr->qualifiers[q].orig_ms,
                            (q < pr->num_qualifiers - 1) ? "," : "");
                }
                fprintf(f_mf, "      ]\n");
                fprintf(f_mf, "    }%s\n", (peak_iter < total_peaks - 1) ? "," : "");

                curr_snap_offset += float32_bytes_per_snap;
                peak_iter++;
            }
        }
        fprintf(f_mf, "  ]\n");
        fprintf(f_mf, "}\n");
        fclose(f_mf);
    }

    // Export report_data.json and report_data.js using direct streaming writes
    char r_data_json_path[4096];
    snprintf(r_data_json_path, sizeof(r_data_json_path), "%s/report_data.json", output_dir);
    FILE* f_rd = fopen(r_data_json_path, "w");

    char r_data_js_path[4096];
    snprintf(r_data_js_path, sizeof(r_data_js_path), "%s/report_data.js", output_dir);
    FILE* f_rd_js = fopen(r_data_js_path, "w");

    if (f_rd || f_rd_js) {
        int num_pts = 1500;
        float* wf_min = (float*)malloc(num_pts * sizeof(float));
        float* wf_max = (float*)malloc(num_pts * sizeof(float));
        int step = len / num_pts;
        if (step < 1) step = 1;

        for (int i = 0; i < num_pts; i++) {
            int p_start = i * step;
            int p_end = p_start + step;
            if (p_start >= len) p_start = len - 1;
            if (p_end > len) p_end = len;

            float min_v = y[p_start];
            float max_v = y[p_start];
            for (int s = p_start; s < p_end; s++) {
                if (y[s] < min_v) min_v = y[s];
                if (y[s] > max_v) max_v = y[s];
            }
            wf_min[i] = min_v;
            wf_max[i] = max_v;
        }

        double global_max_pos = 1.0;
        double global_min_neg = -1.0;
        for (int b = 0; b < MAX_BANDS; b++) {
            for (int k = 0; k < res2.bands[b].num_peaks; k++) {
                double s = res2.bands[b].peaks[k].total_score;
                if (s > 0 && (s > global_max_pos || global_max_pos == 1.0)) global_max_pos = s;
                if (s < 0 && (s < global_min_neg || global_min_neg == -1.0)) global_min_neg = s;
            }
        }

        int num_files = (f_rd && f_rd_js) ? 2 : 1;
        if (f_rd_js) fprintf(f_rd_js, "window.reportData = ");

        for (int file_i = 0; file_i < num_files; file_i++) {
            FILE* fp = (file_i == 0 && f_rd) ? f_rd : f_rd_js;
            if (!fp) continue;

            fprintf(fp, "{\n");
            fprintf(fp, "  \"pass2_win_ms\": %d,\n", pass2_win_ms);
            fprintf(fp, "  \"frame_duration_ms\": %.6f,\n", 1000.0 * (double)(sr * 0.001) / (double)sr);
            fprintf(fp, "  \"total_dur_s\": %.4f,\n", total_duration_ms / 1000.0);
            fprintf(fp, "  \"best_bar_length\": %.2f,\n", best_bar_length_ms);
            fprintf(fp, "  \"global_max_pos_score\": %.6f,\n", global_max_pos);
            fprintf(fp, "  \"global_min_neg_score\": %.6f,\n", global_min_neg);
            fprintf(fp, "  \"tolerance_ms\": %.4f,\n", res2.tolerance);

            // Waveform min/max
            fprintf(fp, "  \"waveform_min\": [");
            for (int i = 0; i < num_pts; i++) fprintf(fp, "%.4f%s", wf_min[i], (i < num_pts - 1) ? "," : "");
            fprintf(fp, "],\n");

            fprintf(fp, "  \"waveform_max\": [");
            for (int i = 0; i < num_pts; i++) fprintf(fp, "%.4f%s", wf_max[i], (i < num_pts - 1) ? "," : "");
            fprintf(fp, "],\n");

            // High point changes
            fprintf(fp, "  \"hp_changes\": [\n");
            for (int i = 0; i < num_hp_changes; i++) {
                fprintf(fp, "    {\"time_s\": %.4f, \"new_value_ms\": %.2f}%s\n",
                        hp_changes[i].time_s, hp_changes[i].new_value_ms, (i < num_hp_changes - 1) ? "," : "");
            }
            fprintf(fp, "  ],\n");

            // Patterns
            fprintf(fp, "  \"patterns\": [\n");
            for (int p = 0; p < num_patterns; p++) {
                fprintf(fp, "    {\n");
                fprintf(fp, "      \"start_ms\": %.2f,\n", patterns[p].start_ms);
                fprintf(fp, "      \"end_ms\": %.2f,\n", patterns[p].end_ms);
                fprintf(fp, "      \"duration_ms\": %.2f,\n", patterns[p].duration_ms);
                fprintf(fp, "      \"rating\": %.6f,\n", patterns[p].rating);
                fprintf(fp, "      \"segments\": [");
                for (int k = 0; k < patterns[p].num_segments; k++) {
                    fprintf(fp, "%d%s", patterns[p].segment_indices[k], (k < patterns[p].num_segments - 1) ? "," : "");
                }
                fprintf(fp, "]\n    }%s\n", (p < num_patterns - 1) ? "," : "");
            }
            fprintf(fp, "  ],\n");

            // Segments Data
            fprintf(fp, "  \"segments_data\": [\n");
            for (int s = 0; s < num_segments; s++) {
                fprintf(fp, "    {\n");
                fprintf(fp, "      \"segment_index\": %d,\n", segments[s].segment_index);
                fprintf(fp, "      \"start_ms\": %.2f,\n", segments[s].start_ms);
                fprintf(fp, "      \"end_ms\": %.2f,\n", segments[s].end_ms);
                fprintf(fp, "      \"rating\": %.6f,\n", segments[s].rating);

                fprintf(fp, "      \"waveform_min\": [");
                for (int p = 0; p < 300; p++) fprintf(fp, "%.4f%s", segments[s].waveform_min[p], (p < 299) ? "," : "");
                fprintf(fp, "],\n");

                fprintf(fp, "      \"waveform_max\": [");
                for (int p = 0; p < 300; p++) fprintf(fp, "%.4f%s", segments[s].waveform_max[p], (p < 299) ? "," : "");
                fprintf(fp, "],\n");

                fprintf(fp, "      \"peaks\": [\n");
                int seg_peak_cnt = 0;
                for (int b = 0; b < MAX_BANDS; b++) {
                    for (int k = 0; k < res2.bands[b].num_peaks; k++) {
                        double peak_ms = res2.bands[b].peaks[k].time * 1000.0;
                        if (peak_ms >= segments[s].start_ms && peak_ms < segments[s].end_ms) {
                            PeakResult* pr = &res2.bands[b].peaks[k];
                            fprintf(fp, "        {\"p_idx\": %d, \"band_idx\": %d, \"time_ms\": %.3f, \"total_score\": %.6f}%s\n",
                                    pr->p_idx, pr->band_idx, peak_ms, pr->total_score, (seg_peak_cnt < segments[s].peak_count - 1) ? "," : "");
                            seg_peak_cnt++;
                        }
                    }
                }
                fprintf(fp, "      ]\n");
                fprintf(fp, "    }%s\n", (s < num_segments - 1) ? "," : "");
            }
            fprintf(fp, "  ],\n");

            // Flat peaks
            fprintf(fp, "  \"pass2_peaks\": [\n");
            int peak_iter = 0;
            size_t c_snap_offset = 0;
            for (int b = 0; b < MAX_BANDS; b++) {
                for (int k = 0; k < res2.bands[b].num_peaks; k++) {
                    PeakResult* pr = &res2.bands[b].peaks[k];
                    double t_ms = pr->time * 1000.0;
                    int f_idx = (int)round(t_ms);
                    if (f_idx < 0) f_idx = 0;
                    if (f_idx >= res2.num_frames) f_idx = res2.num_frames - 1;
                    double dem_line = (res2.demarcation_lines && res2.num_frames > 0) ? res2.demarcation_lines[f_idx] : 0.0;

                    fprintf(fp, "    {\n");
                    fprintf(fp, "      \"p_idx\": %d,\n", pr->p_idx);
                    fprintf(fp, "      \"band_idx\": %d,\n", pr->band_idx);
                    fprintf(fp, "      \"time_s\": %.6f,\n", pr->time);
                    fprintf(fp, "      \"time_ms\": %.3f,\n", t_ms);
                    fprintf(fp, "      \"total_score\": %.6f,\n", pr->total_score);
                    fprintf(fp, "      \"demarcation_line\": %.6f,\n", dem_line);
                    fprintf(fp, "      \"snap_offset\": %llu,\n", (unsigned long long)c_snap_offset);
                    fprintf(fp, "      \"snap_len\": %d,\n", snap_len);

                    fprintf(fp, "      \"qualifiers\": [\n");
                    for (int q = 0; q < pr->num_qualifiers; q++) {
                        fprintf(fp, "        {\"ms\": %.3f, \"val\": %.6f, \"orig_ms\": %.3f}%s\n",
                                pr->qualifiers[q].ms, pr->qualifiers[q].val, pr->qualifiers[q].orig_ms,
                                (q < pr->num_qualifiers - 1) ? "," : "");
                    }
                    fprintf(fp, "      ]\n");
                    fprintf(fp, "    }%s\n", (peak_iter < total_peaks - 1) ? "," : "");

                    c_snap_offset += float32_bytes_per_snap;
                    peak_iter++;
                }
            }
            fprintf(fp, "  ]\n");
            fprintf(fp, "}\n");
            if (fp == f_rd_js) fprintf(fp, ";\n");
        }

        if (f_rd) fclose(f_rd);
        if (f_rd_js) fclose(f_rd_js);
        free(wf_min);
        free(wf_max);
    }

    free(segments);

    // Export HTML Report File with full JS renderer functions
    char html_filepath[4096];
    snprintf(html_filepath, sizeof(html_filepath), "%s/%s_pattern_analysis.html", output_dir, stem_name);
    FILE* f_html = fopen(html_filepath, "w");
    if (f_html) {
        fprintf(f_html, "<!DOCTYPE html>\n");
        fprintf(f_html, "<html lang=\"en\">\n");
        fprintf(f_html, "<head>\n");
        fprintf(f_html, "    <meta charset=\"UTF-8\">\n");
        fprintf(f_html, "    <title>Pattern Analysis Report - %s</title>\n", stem_name);
        fprintf(f_html, "    <script src=\"report_data.js\"></script>\n");
        fprintf(f_html, "    <script src=\"snapshots.js\"></script>\n");
        fprintf(f_html, "    <style>\n");
        fprintf(f_html, "        body { font-family: 'Segoe UI', sans-serif; background-color: #f8f9fa; color: #2c3e50; margin: 0; padding: 20px; }\n");
        fprintf(f_html, "        .container { max-width: 1200px; margin: 0 auto; background: #ffffff; padding: 25px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); }\n");
        fprintf(f_html, "        h1 { color: #2c3e50; border-bottom: 2px solid #ecf0f1; padding-bottom: 10px; margin-top: 0; }\n");
        fprintf(f_html, "        .metrics-card { display: flex; gap: 20px; margin-bottom: 25px; }\n");
        fprintf(f_html, "        .metric-box { flex: 1; background: #eef2f7; padding: 15px; border-radius: 8px; text-align: center; border-left: 4px solid #3498db; }\n");
        fprintf(f_html, "        .metric-value { font-size: 22px; font-weight: bold; color: #2980b9; }\n");
        fprintf(f_html, "        .metric-label { font-size: 13px; color: #7f8c8d; text-transform: uppercase; }\n");
        fprintf(f_html, "        .section-title { font-size: 18px; font-weight: bold; margin-top: 25px; margin-bottom: 15px; color: #34495e; }\n");
        fprintf(f_html, "        .audio-controls { margin: 20px 0; text-align: center; }\n");
        fprintf(f_html, "        audio { width: 100%%; max-width: 800px; outline: none; }\n");
        fprintf(f_html, "        .canvas-container { position: relative; margin-top: 15px; border: 1px solid #dcdde1; border-radius: 8px; overflow: hidden; background: #ffffff; cursor: pointer; }\n");
        fprintf(f_html, "        canvas { display: block; width: 100%%; }\n");
        fprintf(f_html, "        #waveformCanvas { height: 250px; }\n");
        fprintf(f_html, "        #historyBufferCanvas { height: 220px; background-color: #ffffff; }\n");
        fprintf(f_html, "        .hint { font-size: 12px; color: #7f8c8d; text-align: center; margin-top: 6px; }\n");
        fprintf(f_html, "        .segment-inspector-container { display: flex; flex-direction: column; gap: 15px; margin-top: 15px; }\n");
        fprintf(f_html, "        .segment-box { border: 1px solid #dcdde1; border-radius: 8px; padding: 12px; background: #fdfdfd; box-shadow: 0 2px 6px rgba(0,0,0,0.04); position: relative; }\n");
        fprintf(f_html, "        .segment-box.active-seg-box { border: 2px solid #3498db; background: #f4f8fc; }\n");
        fprintf(f_html, "        .segment-box-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }\n");
        fprintf(f_html, "        .seg-title { font-size: 14px; font-weight: bold; color: #2c3e50; }\n");
        fprintf(f_html, "        .seg-rating { font-size: 14px; font-weight: bold; color: #27ae60; background: #e8f8f5; padding: 4px 10px; border-radius: 12px; border: 1px solid #2ecc71; }\n");
        fprintf(f_html, "        .seg-canvas { height: 120px; border: 1px solid #e1e8ed; border-radius: 6px; background: #ffffff; cursor: pointer; }\n");
        fprintf(f_html, "    </style>\n");
        fprintf(f_html, "</head>\n");
        fprintf(f_html, "<body>\n");
        fprintf(f_html, "<div class=\"container\">\n");
        fprintf(f_html, "    <h1>Audio Pattern Analysis Report</h1>\n");
        fprintf(f_html, "    <div style=\"font-size: 14px; color: #7f8c8d; margin-bottom: 20px;\">\n");
        fprintf(f_html, "        File: <strong>%s</strong> | Total Duration: <strong>%.2fs</strong>\n", stem_name, total_duration_ms / 1000.0);
        fprintf(f_html, "    </div>\n");

        fprintf(f_html, "    <div class=\"metrics-card\">\n");
        fprintf(f_html, "        <div class=\"metric-box\">\n");
        fprintf(f_html, "            <div class=\"metric-value\">%.2f ms</div>\n", best_bar_length_ms);
        fprintf(f_html, "            <div class=\"metric-label\">Segment Length (Longest High Point)</div>\n");
        fprintf(f_html, "        </div>\n");
        fprintf(f_html, "        <div class=\"metric-box\">\n");
        fprintf(f_html, "            <div class=\"metric-value\">%d</div>\n", num_hp_changes);
        fprintf(f_html, "            <div class=\"metric-label\">High Point Change Events</div>\n");
        fprintf(f_html, "        </div>\n");
        fprintf(f_html, "        <div class=\"metric-box\">\n");
        fprintf(f_html, "            <div class=\"metric-value\">%d</div>\n", num_patterns);
        fprintf(f_html, "            <div class=\"metric-label\">Patterns Identified</div>\n");
        fprintf(f_html, "        </div>\n");
        fprintf(f_html, "    </div>\n");

        fprintf(f_html, "    <div class=\"section-title\">1. Interactive Audio Waveform, Pattern Map & Cumulative History High Point Changes</div>\n");
        fprintf(f_html, "    <div class=\"audio-controls\">\n");
        fprintf(f_html, "        <audio id=\"audioPlayer\" controls src=\"../%s.wav\"></audio>\n", stem_name);
        fprintf(f_html, "    </div>\n");

        fprintf(f_html, "    <div class=\"canvas-container\">\n");
        fprintf(f_html, "        <canvas id=\"waveformCanvas\" width=\"1150\" height=\"250\"></canvas>\n");
        fprintf(f_html, "    </div>\n");
        fprintf(f_html, "    <div class=\"hint\">💡 Click anywhere on the waveform graph above to seek to that point in the song and play audio. Red markers denote points where the cumulative transience history high point changed.</div>\n");

        fprintf(f_html, "    <div class=\"section-title\">2. Zoomed-In Segment Inspector (Vertically Stacked)</div>\n");
        fprintf(f_html, "    <div class=\"segment-inspector-container\">\n");
        fprintf(f_html, "        <div class=\"segment-box\" id=\"boxPrev\">\n");
        fprintf(f_html, "            <div class=\"segment-box-header\">\n");
        fprintf(f_html, "                <span class=\"seg-title\" id=\"titlePrev\">Previous Segment (Seg -1)</span>\n");
        fprintf(f_html, "                <span class=\"seg-rating\" id=\"ratingPrev\">Average Rating: --</span>\n");
        fprintf(f_html, "            </div>\n");
        fprintf(f_html, "            <canvas id=\"canvasPrev\" class=\"seg-canvas\" width=\"1150\" height=\"120\"></canvas>\n");
        fprintf(f_html, "        </div>\n");
        fprintf(f_html, "        <div class=\"segment-box active-seg-box\" id=\"boxCurr\">\n");
        fprintf(f_html, "            <div class=\"segment-box-header\">\n");
        fprintf(f_html, "                <span class=\"seg-title\" id=\"titleCurr\">Current Playing Segment (Seg 0)</span>\n");
        fprintf(f_html, "                <span class=\"seg-rating\" id=\"ratingCurr\">Average Rating: --</span>\n");
        fprintf(f_html, "            </div>\n");
        fprintf(f_html, "            <canvas id=\"canvasCurr\" class=\"seg-canvas\" width=\"1150\" height=\"120\"></canvas>\n");
        fprintf(f_html, "        </div>\n");
        fprintf(f_html, "        <div class=\"segment-box\" id=\"boxNext\">\n");
        fprintf(f_html, "            <div class=\"segment-box-header\">\n");
        fprintf(f_html, "                <span class=\"seg-title\" id=\"titleNext\">Next Segment (Seg 1)</span>\n");
        fprintf(f_html, "                <span class=\"seg-rating\" id=\"ratingNext\">Average Rating: --</span>\n");
        fprintf(f_html, "            </div>\n");
        fprintf(f_html, "            <canvas id=\"canvasNext\" class=\"seg-canvas\" width=\"1150\" height=\"120\"></canvas>\n");
        fprintf(f_html, "        </div>\n");
        fprintf(f_html, "    </div>\n");
        fprintf(f_html, "    <div class=\"hint\">💡 Click anywhere on any of the segment graphs above to jump to that exact point in the song and play audio.</div>\n");

        fprintf(f_html, "    <div class=\"section-title\">3. Accumulated History Buffer</div>\n");
        fprintf(f_html, "    <div class=\"canvas-container\" style=\"cursor: crosshair; background: #ffffff;\">\n");
        fprintf(f_html, "        <canvas id=\"historyBufferCanvas\" width=\"1150\" height=\"220\"></canvas>\n");
        fprintf(f_html, "    </div>\n");
        fprintf(f_html, "    <div class=\"hint\">💡 Select a section to zoom in on. Right-click anywhere on the graph to zoom back out to the full duration. Graph updates in real time during audio playback.</div>\n");
        fprintf(f_html, "</div>\n");

        fprintf(f_html, "<script>\n");
        fprintf(f_html, "    let pass2WinMs = %d;\n", pass2_win_ms);
        fprintf(f_html, "    let frameDurationMs = %.6f;\n", 1000.0 * (double)(sr * 0.001) / (double)sr);
        fprintf(f_html, "    let waveformMin = [];\n");
        fprintf(f_html, "    let waveformMax = [];\n");
        fprintf(f_html, "    let totalDurationS = %.4f;\n", total_duration_ms / 1000.0);
        fprintf(f_html, "    let bestBarLengthMs = %.2f;\n", best_bar_length_ms);
        fprintf(f_html, "    let patterns = [];\n");
        fprintf(f_html, "    let segmentsData = [];\n");
        fprintf(f_html, "    let hpChanges = [];\n");
        fprintf(f_html, "    let globalMaxPosScore = 1.0;\n");
        fprintf(f_html, "    let globalMinNegScore = -1.0;\n");
        fprintf(f_html, "    let toleranceMs = %.4f;\n", res2.tolerance);
        fprintf(f_html, "    let pass2Peaks = [];\n\n");

        fprintf(f_html, "    function applyReportData(data) {\n");
        fprintf(f_html, "        if (!data) return;\n");
        fprintf(f_html, "        if (data.pass2_win_ms !== undefined) pass2WinMs = data.pass2_win_ms;\n");
        fprintf(f_html, "        if (data.frame_duration_ms !== undefined) frameDurationMs = data.frame_duration_ms;\n");
        fprintf(f_html, "        if (data.waveform_min) waveformMin = data.waveform_min;\n");
        fprintf(f_html, "        if (data.waveform_max) waveformMax = data.waveform_max;\n");
        fprintf(f_html, "        if (data.total_dur_s !== undefined) totalDurationS = data.total_dur_s;\n");
        fprintf(f_html, "        if (data.best_bar_length !== undefined) bestBarLengthMs = data.best_bar_length;\n");
        fprintf(f_html, "        if (data.patterns) patterns = data.patterns;\n");
        fprintf(f_html, "        if (data.segments_data) segmentsData = data.segments_data;\n");
        fprintf(f_html, "        if (data.hp_changes) hpChanges = data.hp_changes;\n");
        fprintf(f_html, "        if (data.global_max_pos_score !== undefined) globalMaxPosScore = data.global_max_pos_score;\n");
        fprintf(f_html, "        if (data.global_min_neg_score !== undefined) globalMinNegScore = data.global_min_neg_score;\n");
        fprintf(f_html, "        if (data.tolerance_ms !== undefined) toleranceMs = data.tolerance_ms;\n");
        fprintf(f_html, "        if (data.pass2_peaks) pass2Peaks = data.pass2_peaks;\n");
        fprintf(f_html, "        renderAll();\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    if (window.reportData) {\n");
        fprintf(f_html, "        applyReportData(window.reportData);\n");
        fprintf(f_html, "    } else {\n");
        fprintf(f_html, "        fetch('report_data.json')\n");
        fprintf(f_html, "            .then(res => res.json())\n");
        fprintf(f_html, "            .then(data => applyReportData(data))\n");
        fprintf(f_html, "            .catch(e => console.log('report_data.json fetch notice:', e));\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    const canvas = document.getElementById('waveformCanvas');\n");
        fprintf(f_html, "    const ctx = canvas.getContext('2d');\n");
        fprintf(f_html, "    const audio = document.getElementById('audioPlayer');\n");
        fprintf(f_html, "    const bufCanvas = document.getElementById('historyBufferCanvas');\n");
        fprintf(f_html, "    const bufCtx = bufCanvas ? bufCanvas.getContext('2d') : null;\n\n");

        fprintf(f_html, "    let zoomStartMs = -pass2WinMs;\n");
        fprintf(f_html, "    let zoomEndMs = 0.0;\n");
        fprintf(f_html, "    let isDraggingBuf = false;\n");
        fprintf(f_html, "    let dragStartX = 0, dragCurrentX = 0;\n\n");

        fprintf(f_html, "    const colors = ['rgba(46, 204, 113, 0.35)', 'rgba(231, 76, 60, 0.35)', 'rgba(155, 89, 182, 0.35)', 'rgba(241, 196, 15, 0.35)'];\n");
        fprintf(f_html, "    const borderColors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f'];\n\n");

        fprintf(f_html, "    function getScoreColor(score, alpha = 1.0) {\n");
        fprintf(f_html, "        if (score === 0) return `rgba(128, 128, 128, ${alpha})`;\n");
        fprintf(f_html, "        let r = 128, g = 128, b = 128;\n");
        fprintf(f_html, "        if (score < 0) {\n");
        fprintf(f_html, "            let t = globalMinNegScore < 0 ? score / globalMinNegScore : 0.0;\n");
        fprintf(f_html, "            t = Math.max(0.0, Math.min(1.0, t));\n");
        fprintf(f_html, "            r = Math.round(128 + (255 - 128) * t);\n");
        fprintf(f_html, "            g = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "            b = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "        } else {\n");
        fprintf(f_html, "            let t = globalMaxPosScore > 0 ? score / globalMaxPosScore : 0.0;\n");
        fprintf(f_html, "            t = Math.max(0.0, Math.min(1.0, t));\n");
        fprintf(f_html, "            r = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "            g = Math.round(128 + (255 - 128) * t);\n");
        fprintf(f_html, "            b = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        return `rgba(${r}, ${g}, ${b}, ${alpha})`;\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function getQualifierColor(val, alpha = 1.0) {\n");
        fprintf(f_html, "        if (val === 0) return `rgba(128, 128, 128, ${alpha})`;\n");
        fprintf(f_html, "        let r = 128, g = 128, b = 128;\n");
        fprintf(f_html, "        if (val < 0) {\n");
        fprintf(f_html, "            let t = Math.max(0.0, Math.min(1.0, val / -1.0));\n");
        fprintf(f_html, "            r = Math.round(128 + (255 - 128) * t);\n");
        fprintf(f_html, "            g = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "            b = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "        } else {\n");
        fprintf(f_html, "            let t = Math.max(0.0, Math.min(1.0, val / 1.0));\n");
        fprintf(f_html, "            r = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "            g = Math.round(128 + (255 - 128) * t);\n");
        fprintf(f_html, "            b = Math.round(128 + (0 - 128) * t);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        return `rgba(${r}, ${g}, ${b}, ${alpha})`;\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function getLatestActivePeak() {\n");
        fprintf(f_html, "        const curTimeMs = (audio.currentTime || 0) * 1000.0;\n");
        fprintf(f_html, "        const windowStartMs = curTimeMs - pass2WinMs;\n");
        fprintf(f_html, "        const activePeaks = pass2Peaks.filter(p => p.time_ms > windowStartMs && p.time_ms <= curTimeMs);\n");
        fprintf(f_html, "        if (activePeaks.length > 0) return activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b));\n");
        fprintf(f_html, "        return null;\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function drawWaveformMap() {\n");
        fprintf(f_html, "        const W = canvas.width, H = canvas.height;\n");
        fprintf(f_html, "        ctx.clearRect(0, 0, W, H);\n");
        fprintf(f_html, "        ctx.fillStyle = '#f8f9fa'; ctx.fillRect(0, 0, W, H);\n");

        fprintf(f_html, "        patterns.forEach((pat, idx) => {\n");
        fprintf(f_html, "            const color = colors[idx %% colors.length];\n");
        fprintf(f_html, "            const borderColor = borderColors[idx %% borderColors.length];\n");
        fprintf(f_html, "            const startX = (pat.start_ms / 1000.0 / totalDurationS) * W;\n");
        fprintf(f_html, "            const endX = (pat.end_ms / 1000.0 / totalDurationS) * W;\n");
        fprintf(f_html, "            ctx.fillStyle = color; ctx.fillRect(startX, 0, endX - startX, H);\n");
        fprintf(f_html, "            ctx.fillStyle = borderColor; ctx.font = 'bold 12px Segoe UI, sans-serif';\n");
        fprintf(f_html, "            ctx.fillText(`Pattern ${idx + 1} (${Math.round(pat.duration_ms)}ms)`, startX + 5, 20);\n");
        fprintf(f_html, "        });\n");

        fprintf(f_html, "        const centerY = H / 2;\n");
        fprintf(f_html, "        const numPts = waveformMin.length;\n");
        fprintf(f_html, "        ctx.lineWidth = 1.2; ctx.strokeStyle = '#2c3e50'; ctx.beginPath();\n");
        fprintf(f_html, "        for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "            const x = (i / numPts) * W;\n");
        fprintf(f_html, "            const minY = centerY - (waveformMin[i] * (H * 0.4));\n");
        fprintf(f_html, "            const maxY = centerY - (waveformMax[i] * (H * 0.4));\n");
        fprintf(f_html, "            ctx.moveTo(x, minY); ctx.lineTo(x, maxY);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        ctx.stroke();\n");

        fprintf(f_html, "        hpChanges.forEach((ch, idx) => {\n");
        fprintf(f_html, "            const x = (ch.time_s / totalDurationS) * W;\n");
        fprintf(f_html, "            ctx.strokeStyle = '#e74c3c'; ctx.lineWidth = 1.5; ctx.setLineDash([3, 3]);\n");
        fprintf(f_html, "            ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke(); ctx.setLineDash([]);\n");
        fprintf(f_html, "            ctx.fillStyle = '#e74c3c'; ctx.font = 'bold 9px Segoe UI, sans-serif';\n");
        fprintf(f_html, "            ctx.fillText(`HP: ${ch.new_value_ms.toFixed(0)}ms`, Math.min(x + 2, W - 60), 45 + (idx %% 4) * 14);\n");
        fprintf(f_html, "        });\n");

        fprintf(f_html, "        if (audio.duration) {\n");
        fprintf(f_html, "            const progress = audio.currentTime / audio.duration;\n");
        fprintf(f_html, "            const cursorX = progress * W;\n");
        fprintf(f_html, "            ctx.strokeStyle = '#e67e22'; ctx.lineWidth = 2.5;\n");
        fprintf(f_html, "            ctx.beginPath(); ctx.moveTo(cursorX, 0); ctx.lineTo(cursorX, H); ctx.stroke();\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function drawSegmentBox(boxId, canvasId, titleId, ratingId, seg, labelPrefix, currentAudioTimeMs) {\n");
        fprintf(f_html, "        const boxElem = document.getElementById(boxId);\n");
        fprintf(f_html, "        const c = document.getElementById(canvasId);\n");
        fprintf(f_html, "        const t = document.getElementById(titleId);\n");
        fprintf(f_html, "        const r = document.getElementById(ratingId);\n");
        fprintf(f_html, "        if (!c) return;\n");
        fprintf(f_html, "        const sCtx = c.getContext('2d');\n");
        fprintf(f_html, "        const W = c.width, H = c.height;\n");
        fprintf(f_html, "        sCtx.clearRect(0, 0, W, H);\n");
        fprintf(f_html, "        if (!seg) {\n");
        fprintf(f_html, "            if (t) t.textContent = `${labelPrefix} Segment (Out of Range)`;\n");
        fprintf(f_html, "            if (r) r.textContent = `Average Rating: N/A`;\n");
        fprintf(f_html, "            sCtx.fillStyle = '#f8f9fa'; sCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "            return;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const patIdx = patterns.findIndex(p => p.segments && p.segments.includes(seg.segment_index));\n");
        fprintf(f_html, "        if (patIdx !== -1) {\n");
        fprintf(f_html, "            const mainColor = borderColors[patIdx %% borderColors.length];\n");
        fprintf(f_html, "            if (boxElem) { boxElem.style.borderColor = mainColor; boxElem.style.borderWidth = '2px'; }\n");
        fprintf(f_html, "            if (t) t.textContent = `${labelPrefix} Segment (Seg ${seg.segment_index} [Pattern ${patIdx + 1}] : ${Math.round(seg.start_ms)} - ${Math.round(seg.end_ms)} ms)`;\n");
        fprintf(f_html, "        } else {\n");
        fprintf(f_html, "            if (t) t.textContent = `${labelPrefix} Segment (Seg ${seg.segment_index} : ${Math.round(seg.start_ms)} - ${Math.round(seg.end_ms)} ms)`;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (r) r.textContent = `Average Rating: ${seg.rating.toFixed(4)}`;\n");
        fprintf(f_html, "        sCtx.fillStyle = '#ffffff'; sCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "        const segDurMs = seg.end_ms - seg.start_ms;\n");
        fprintf(f_html, "        if (seg.peaks && seg.peaks.length > 0) {\n");
        fprintf(f_html, "            seg.peaks.forEach(p => {\n");
        fprintf(f_html, "                const relMs = p.time_ms - seg.start_ms;\n");
        fprintf(f_html, "                const x = (relMs / segDurMs) * W;\n");
        fprintf(f_html, "                sCtx.strokeStyle = getScoreColor(p.total_score, 0.55);\n");
        fprintf(f_html, "                sCtx.lineWidth = 1.8; sCtx.setLineDash([4, 4]);\n");
        fprintf(f_html, "                sCtx.beginPath(); sCtx.moveTo(x, 0); sCtx.lineTo(x, H); sCtx.stroke(); sCtx.setLineDash([]);\n");
        fprintf(f_html, "            });\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const centerY = H / 2;\n");
        fprintf(f_html, "        sCtx.strokeStyle = 'rgba(189, 195, 199, 0.4)'; sCtx.beginPath(); sCtx.moveTo(0, centerY); sCtx.lineTo(W, centerY); sCtx.stroke();\n");
        fprintf(f_html, "        if (seg.waveform_min && seg.waveform_min.length > 0) {\n");
        fprintf(f_html, "            const numPts = seg.waveform_min.length;\n");
        fprintf(f_html, "            sCtx.lineWidth = 1.0; sCtx.strokeStyle = '#34495e'; sCtx.beginPath();\n");
        fprintf(f_html, "            for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "                const x = (i / numPts) * W;\n");
        fprintf(f_html, "                const minY = centerY - (seg.waveform_min[i] * (H * 0.38));\n");
        fprintf(f_html, "                const maxY = centerY - (seg.waveform_max[i] * (H * 0.38));\n");
        fprintf(f_html, "                sCtx.moveTo(x, minY); sCtx.lineTo(x, maxY);\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "            sCtx.stroke();\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (currentAudioTimeMs >= seg.start_ms && currentAudioTimeMs <= seg.end_ms) {\n");
        fprintf(f_html, "            const cursorX = ((currentAudioTimeMs - seg.start_ms) / segDurMs) * W;\n");
        fprintf(f_html, "            sCtx.strokeStyle = '#e67e22'; sCtx.lineWidth = 2.5; sCtx.beginPath(); sCtx.moveTo(cursorX, 0); sCtx.lineTo(cursorX, H); sCtx.stroke();\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function updateSegmentInspector() {\n");
        fprintf(f_html, "        const curTimeMs = (audio.currentTime || 0) * 1000.0;\n");
        fprintf(f_html, "        const curSegIdx = Math.floor(curTimeMs / bestBarLengthMs);\n");
        fprintf(f_html, "        const prevSeg = segmentsData.find(s => s.segment_index === curSegIdx - 1);\n");
        fprintf(f_html, "        const currSeg = segmentsData.find(s => s.segment_index === curSegIdx);\n");
        fprintf(f_html, "        const nextSeg = segmentsData.find(s => s.segment_index === curSegIdx + 1);\n");
        fprintf(f_html, "        drawSegmentBox('boxPrev', 'canvasPrev', 'titlePrev', 'ratingPrev', prevSeg, 'Previous', curTimeMs);\n");
        fprintf(f_html, "        drawSegmentBox('boxCurr', 'canvasCurr', 'titleCurr', 'ratingCurr', currSeg, 'Current Playing', curTimeMs);\n");
        fprintf(f_html, "        drawSegmentBox('boxNext', 'canvasNext', 'titleNext', 'ratingNext', nextSeg, 'Next', curTimeMs);\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function drawHistoryBuffer() {\n");
        fprintf(f_html, "        if (!bufCanvas || !bufCtx) return;\n");
        fprintf(f_html, "        const W = bufCanvas.width, H = bufCanvas.height;\n");
        fprintf(f_html, "        bufCtx.clearRect(0, 0, W, H);\n");
        fprintf(f_html, "        bufCtx.fillStyle = '#ffffff'; bufCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "        const curTimeMs = (audio.currentTime || 0) * 1000.0;\n");
        fprintf(f_html, "        const padLeft = 65, padRight = 35, padTop = 30, padBottom = 35;\n");
        fprintf(f_html, "        const graphW = W - padLeft - padRight, graphH = H - padTop - padBottom;\n");
        fprintf(f_html, "        const windowStartMs = curTimeMs - pass2WinMs;\n");
        fprintf(f_html, "        const activePeaks = pass2Peaks.filter(p => p.time_ms > windowStartMs && p.time_ms <= curTimeMs);\n");
        fprintf(f_html, "        const latestPeak = (activePeaks.length > 0) ? activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b)) : null;\n");
        fprintf(f_html, "        let accumulatedBuffer = null;\n");
        fprintf(f_html, "        if (latestPeak && window.snapshotsArrayBuffer && latestPeak.snap_offset !== undefined) {\n");
        fprintf(f_html, "            const floatLen = latestPeak.snap_len || (pass2WinMs + 1);\n");
        fprintf(f_html, "            accumulatedBuffer = new Float32Array(window.snapshotsArrayBuffer, latestPeak.snap_offset, floatLen);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const numBufPts = accumulatedBuffer ? accumulatedBuffer.length : (Math.round(pass2WinMs) + 1);\n");
        fprintf(f_html, "        if (!accumulatedBuffer) accumulatedBuffer = new Float32Array(numBufPts);\n");
        fprintf(f_html, "        const snapBinMs = frameDurationMs || ((numBufPts > 1) ? (pass2WinMs / (numBufPts - 1)) : 1.0);\n");
        fprintf(f_html, "        let curMax = 0, curMin = Infinity;\n");
        fprintf(f_html, "        for (let i = 0; i < numBufPts; i++) {\n");
        fprintf(f_html, "            const sampleMs = (i - (numBufPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "            if (sampleMs >= zoomStartMs && sampleMs <= zoomEndMs && sampleMs <= -99.0 + 1e-5) {\n");
        fprintf(f_html, "                const val = accumulatedBuffer[i];\n");
        fprintf(f_html, "                if (val > curMax) curMax = val;\n");
        fprintf(f_html, "                if (val < curMin) curMin = val;\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (curMax <= 0) curMax = 1.0;\n");
        fprintf(f_html, "        const yMax = curMax * 1.1;\n");
        fprintf(f_html, "        bufCtx.strokeStyle = '#dcdde1'; bufCtx.lineWidth = 1; bufCtx.strokeRect(padLeft, padTop, graphW, graphH);\n");
        fprintf(f_html, "        const zoomSpan = zoomEndMs - zoomStartMs;\n");
        fprintf(f_html, "        bufCtx.save(); bufCtx.beginPath(); bufCtx.rect(padLeft, padTop, graphW, graphH); bufCtx.clip();\n");
        fprintf(f_html, "        bufCtx.strokeStyle = '#3498db'; bufCtx.lineWidth = 2.0; bufCtx.fillStyle = 'rgba(52, 152, 219, 0.15)';\n");
        fprintf(f_html, "        bufCtx.beginPath(); let firstPt = true;\n");
        fprintf(f_html, "        for (let i = 0; i < numBufPts; i++) {\n");
        fprintf(f_html, "            const sampleMs = (i - (numBufPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "            if (sampleMs > -99.0 + 1e-5) continue;\n");
        fprintf(f_html, "            const x = padLeft + ((sampleMs - zoomStartMs) / zoomSpan) * graphW;\n");
        fprintf(f_html, "            const y = padTop + graphH - (accumulatedBuffer[i] / yMax) * graphH;\n");
        fprintf(f_html, "            if (firstPt) { bufCtx.moveTo(x, y); firstPt = false; } else { bufCtx.lineTo(x, y); }\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        bufCtx.stroke(); bufCtx.restore();\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function renderAll() {\n");
        fprintf(f_html, "        drawWaveformMap();\n");
        fprintf(f_html, "        updateSegmentInspector();\n");
        fprintf(f_html, "        drawHistoryBuffer();\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    if (window.snapshotsBase64) {\n");
        fprintf(f_html, "        try {\n");
        fprintf(f_html, "            const binaryString = atob(window.snapshotsBase64);\n");
        fprintf(f_html, "            const bytes = new Uint8Array(binaryString.length);\n");
        fprintf(f_html, "            for (let i = 0; i < binaryString.length; i++) bytes[i] = binaryString.charCodeAt(i);\n");
        fprintf(f_html, "            window.snapshotsArrayBuffer = bytes.buffer;\n");
        fprintf(f_html, "        } catch (e) { console.log('Base64 decode error:', e); }\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    renderAll();\n");
        fprintf(f_html, "    audio.addEventListener('timeupdate', renderAll);\n");
        fprintf(f_html, "    audio.addEventListener('play', renderAll);\n");
        fprintf(f_html, "    audio.addEventListener('pause', renderAll);\n");
        fprintf(f_html, "</script>\n");
        fprintf(f_html, "</body>\n");
        fprintf(f_html, "</html>\n");
        fclose(f_html);
    }

    analyzer_free_analysis(&res2);
    return 1;
}

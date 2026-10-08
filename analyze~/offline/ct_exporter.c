#include "ct_exporter.h"
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define mkdir_cross(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#define mkdir_cross(dir) mkdir(dir, 0755)
#endif

int mkdir_p(const char* path) {
    if (!path || !*path) return 0;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = '\0';
        len--;
    }

    char* p = NULL;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (strlen(tmp) > 0) {
                mkdir_cross(tmp);
            }
            *p = '/';
        }
    }
    return mkdir_cross(tmp);
}

typedef struct {
    int index;
    double loop_point_ms;
    double arbitrary_pad_ms;
} PassInfo;

static int compare_passes(const void* a, const void* b) {
    const PassInfo* pa = (const PassInfo*)a;
    const PassInfo* pb = (const PassInfo*)b;
    if (pa->index != pb->index) {
        return pa->index - pb->index;
    }
    if (pa->loop_point_ms < pb->loop_point_ms) return -1;
    if (pa->loop_point_ms > pb->loop_point_ms) return 1;
    return 0;
}

static int iequals_ext(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void url_encode_filename(const char* src, char* dst, size_t dst_size) {
    size_t d = 0;
    while (*src && d + 3 < dst_size) {
        unsigned char c = (unsigned char)*src;
        if (c == '#') {
            dst[d++] = '%';
            dst[d++] = '2';
            dst[d++] = '3';
        } else if (c == '%') {
            dst[d++] = '%';
            dst[d++] = '2';
            dst[d++] = '5';
        } else if (c == '?') {
            dst[d++] = '%';
            dst[d++] = '3';
            dst[d++] = 'F';
        } else {
            dst[d++] = c;
        }
        src++;
    }
    dst[d] = '\0';
}

static int is_text_file_ext(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return 0;
    if (iequals_ext(dot, ".txt") || iequals_ext(dot, ".json") || iequals_ext(dot, ".csv") ||
        iequals_ext(dot, ".log") || iequals_ext(dot, ".xml")  || iequals_ext(dot, ".text")) {
        return 1;
    }
    return 0;
}

static void parse_passes_in_dir(const char* output_dir, PassInfo* out_passes, int* out_total_passes) {
    out_passes[0].index = 0;
    out_passes[0].loop_point_ms = 0.0;
    out_passes[0].arbitrary_pad_ms = 0.0;
    *out_total_passes = 1;

    DIR* dir = opendir(output_dir);
    if (!dir) return;

    char passes_filepath[4096] = "";
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        char lower_name[256];
        size_t n_len = strlen(name);
        if (n_len >= sizeof(lower_name)) n_len = sizeof(lower_name) - 1;
        for (size_t i = 0; i < n_len; i++) lower_name[i] = (char)tolower((unsigned char)name[i]);
        lower_name[n_len] = '\0';

        if (strstr(lower_name, "passes") != NULL && is_text_file_ext(name)) {
            snprintf(passes_filepath, sizeof(passes_filepath), "%s" PATH_SEP_STR "%s", output_dir, name);
            break;
        }
    }
    closedir(dir);

    if (passes_filepath[0] == '\0') return;

    FILE* fp = fopen(passes_filepath, "r");
    if (!fp) return;

    PassInfo raw_passes[256];
    int raw_count = 0;
    char line[1024];

    while (fgets(line, sizeof(line), fp)) {
        char* ptr = line;
        while (*ptr && isspace((unsigned char)*ptr)) ptr++;
        if (*ptr == '\0' || *ptr == '#' || *ptr == '/') continue;

        int idx = 0;
        double lp_ms = 0.0;
        double pad_ms = 0.0;

        int count = sscanf(ptr, "%d , %lf , %lf", &idx, &lp_ms, &pad_ms);
        if (count < 2) {
            count = sscanf(ptr, "%d %lf %lf", &idx, &lp_ms, &pad_ms);
        }

        if (count >= 2) {
            if (raw_count < 256) {
                raw_passes[raw_count].index = idx;
                raw_passes[raw_count].loop_point_ms = lp_ms;
                raw_passes[raw_count].arbitrary_pad_ms = (count >= 3) ? pad_ms : 0.0;
                raw_count++;
            }
        }
    }
    fclose(fp);

    if (raw_count > 0) {
        qsort(raw_passes, raw_count, sizeof(PassInfo), compare_passes);
        for (int i = 0; i < raw_count; i++) {
            out_passes[*out_total_passes] = raw_passes[i];
            (*out_total_passes)++;
        }
    }
}

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
    if (window_ms > 15000) window_ms = 15000;
    if (window_ms < 5000) window_ms = 5000;

    FullAnalysisResult res;
    int success = analyzer_batch_analyze(y, len, sr, window_ms, 1, &res);
    if (!success) return 0;

    FILE* f = fopen(output_filepath, "wb");
    if (!f) {
        analyzer_free_analysis(&res);
        return 0;
    }

    int32_t total_peaks = 0;
    for (int b = 0; b < res.num_bands; b++) total_peaks += res.bands[b].num_peaks;

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
    for (int b = 0; b < res.num_bands; b++) {
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
    if (!analyzer_batch_analyze(y, len, sr, pass1_window_ms, 1, &res1)) return 0;

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

    // Record and sum normalized cumulative transience buffer contours every 19ms (19 frames)
    #define MAX_CONTOUR_LEN 15001
    int contour_len = pass1_window_ms + 1;
    if (contour_len > MAX_CONTOUR_LEN) contour_len = MAX_CONTOUR_LEN;

    double* accumulated_contour = (double*)calloc(contour_len, sizeof(double));
    int contour_sample_count = 0;

    TransientAnalyzer* contour_analyzer = analyzer_create(1.0, NULL, NULL, NULL, NULL, pass1_window_ms, 1);
    if (contour_analyzer) {
        analyzer_set_sample_rate(contour_analyzer, sr);
        int hop = (int)(sr * 0.001);
        int step = hop * 19; // Record buffer contour every 19 ms (19 frames)

        for (int last_t = 0; last_t < len; last_t += step) {
            int act_s = last_t - (int)(sr * 0.2);
            int win_s = act_s - (int)(sr * (pass1_window_ms / 1000.0));
            if (win_s < 0) win_s = 0;

            ChunkAnalysisResult* res_chunk = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));
            if (res_chunk) {
                float* push_ptr = (float*)calloc(step, sizeof(float));
                int chunk_len = step;
                if (last_t + step > len) chunk_len = len - last_t;
                if (chunk_len > 0) {
                    memcpy(push_ptr, y + last_t, sizeof(float) * chunk_len);
                }
                analyzer_analyze_chunk(contour_analyzer, push_ptr, step, sr, win_s / hop, act_s / hop, res_chunk);
                free(push_ptr);
                free(res_chunk);
            }

            double* cur_buf = analyzer_get_buffer(contour_analyzer);
            if (cur_buf && accumulated_contour) {
                for (int k = 0; k < contour_len; k++) {
                    accumulated_contour[k] += cur_buf[k];
                }
                contour_sample_count++;
            }
        }
        analyzer_destroy(contour_analyzer);
    }

    // Determine segment length as the high point (global maximum) of accumulated_contour
    // Index 0 represents -15,000 ms (oldest sample in rolling window), and index (contour_len - 1) represents 0 ms (current peak).
    // The lag time before the current peak corresponds to: (contour_len - 1 - k) ms.
    int m_len = contour_len - 99; // Exclude last 99ms self-referential peak region
    double max_contour_val = -1e9;
    int best_lag_ms = 1000;

    if (m_len > 0 && accumulated_contour) {
        for (int k = 0; k < m_len; k++) {
            if (accumulated_contour[k] > max_contour_val) {
                max_contour_val = accumulated_contour[k];
                best_lag_ms = (contour_len - 1) - k;
            }
        }
    }

    double best_bar_length_ms = (double)best_lag_ms;
    if (best_bar_length_ms <= 0.0) best_bar_length_ms = 1000.0;

    double dom_start_ms = best_bar_length_ms - 50.0;
    double dom_end_ms = best_bar_length_ms + 50.0;

    // Find the longest contiguous run of high points falling within range in Pass 1
    double longest_hp_start_s = 0.0;
    double longest_hp_end_s = 0.0;
    double max_run_dur = 0.0;

    double cur_run_start_s = -1.0;
    double cur_run_end_s = -1.0;

    for (int i = 0; i < num_frames; i++) {
        double raw_hp = res1.highest_peaks_ms[i];
        if (raw_hp != -999.0 && fabs(raw_hp) >= dom_start_ms && fabs(raw_hp) <= dom_end_ms) {
            if (cur_run_start_s < 0.0) {
                cur_run_start_s = res1.times[i];
            }
            cur_run_end_s = res1.times[i] + frame_dt_s;
        } else {
            if (cur_run_start_s >= 0.0) {
                double run_dur = cur_run_end_s - cur_run_start_s;
                if (run_dur > max_run_dur) {
                    max_run_dur = run_dur;
                    longest_hp_start_s = cur_run_start_s;
                    longest_hp_end_s = cur_run_end_s;
                }
                cur_run_start_s = -1.0;
                cur_run_end_s = -1.0;
            }
        }
    }
    if (cur_run_start_s >= 0.0) {
        double run_dur = cur_run_end_s - cur_run_start_s;
        if (run_dur > max_run_dur) {
            max_run_dur = run_dur;
            longest_hp_start_s = cur_run_start_s;
            longest_hp_end_s = cur_run_end_s;
        }
    }

    if (max_run_dur <= 0.0) {
        longest_hp_start_s = 0.0;
        longest_hp_end_s = (num_frames > 0) ? res1.times[num_frames - 1] : 0.0;
    }

    double midpoint_s = (longest_hp_start_s + longest_hp_end_s) / 2.0;
    int hop = (int)(sr * 0.001);
    int midpoint_frame = (hop > 0) ? (int)round(midpoint_s * (double)sr / (double)hop) : 0;
    if (midpoint_frame < 0) midpoint_frame = 0;
    if (midpoint_frame >= num_frames) midpoint_frame = (num_frames > 0) ? (num_frames - 1) : 0;

    double* midpoint_buffer = (double*)calloc(pass1_window_ms + 1, sizeof(double));
    double midpoint_demarcation_line = 0.0;

    TransientAnalyzer* mid_analyzer = analyzer_create(1.0, NULL, NULL, NULL, NULL, pass1_window_ms, 1);
    if (mid_analyzer) {
        analyzer_set_sample_rate(mid_analyzer, sr);
        int target_sample = (int)round(midpoint_s * (double)sr);
        if (target_sample > len) target_sample = len;
        int step = hop * 100;

        for (int last_t = 0; last_t < target_sample; last_t += step) {
            int act_s = last_t - (int)(sr * 0.2);
            int win_s = act_s - (int)(sr * (pass1_window_ms / 1000.0));
            if (win_s < 0) win_s = 0;

            ChunkAnalysisResult* res_chunk = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));
            if (res_chunk) {
                float* push_ptr = (float*)calloc(step, sizeof(float));
                int chunk_len = step;
                if (last_t + step > target_sample) chunk_len = target_sample - last_t;
                if (chunk_len > 0) {
                    memcpy(push_ptr, y + last_t, sizeof(float) * chunk_len);
                }
                analyzer_analyze_chunk(mid_analyzer, push_ptr, step, sr, win_s / hop, act_s / hop, res_chunk);
                free(push_ptr);
                free(res_chunk);
            }
        }

        double* cur_buf = analyzer_get_buffer(mid_analyzer);
        if (cur_buf && midpoint_buffer) {
            memcpy(midpoint_buffer, cur_buf, sizeof(double) * (pass1_window_ms + 1));
        }
        if (res1.demarcation_lines && midpoint_frame < num_frames) {
            midpoint_demarcation_line = res1.demarcation_lines[midpoint_frame];
        }
        analyzer_destroy(mid_analyzer);
    }

    analyzer_free_analysis(&res1);

    // Pass 2: Re-analyze with window_ms = clamp(best_bar_length * 2, 5000, 15000)
    int pass2_win_ms = (int)round(best_bar_length_ms * 2.0);
    if (pass2_win_ms > 15000) pass2_win_ms = 15000;
    if (pass2_win_ms < 5000) pass2_win_ms = 5000;

    FullAnalysisResult res2;
    if (!analyzer_batch_analyze(y, len, sr, pass2_win_ms, 1, &res2)) return 0;

    // Export .ctbin binary file for Pass 2
    char ctbin_path[4096];
    snprintf(ctbin_path, sizeof(ctbin_path), "%s" PATH_SEP_STR "%s.ctbin", output_dir, stem_name);
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
        for (int b = 0; b < res2.num_bands; b++) {
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

    // Ensure [loops] and [stems] subdirectories exist inside output_dir
    char loops_dir[4096];
    snprintf(loops_dir, sizeof(loops_dir), "%s" PATH_SEP_STR "[loops]", output_dir);
    mkdir_p(loops_dir);

    char stems_dir[4096];
    snprintf(stems_dir, sizeof(stems_dir), "%s" PATH_SEP_STR "[stems]", output_dir);
    mkdir_p(stems_dir);

    // Parse passes text file if present
    PassInfo passes[257];
    int total_passes = 0;
    parse_passes_in_dir(output_dir, passes, &total_passes);

    // Cut & Save identified pattern WAV files directly using dr_wav
    for (int p = 0; p < num_patterns; p++) {
        // 1. Loopable WAV file in [loops]
        char pat_wav_path[4096];
        snprintf(pat_wav_path, sizeof(pat_wav_path), "%s" PATH_SEP_STR "[loops]" PATH_SEP_STR "pattern_%d.wav", output_dir, p + 1);

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

        // 2. Stems WAV file (aligned with pass loop point and arbitrary pad)
        int assigned_pass_idx = 0;
        for (int i = total_passes - 1; i >= 0; i--) {
            if (patterns[p].start_ms >= passes[i].loop_point_ms) {
                assigned_pass_idx = i;
                break;
            }
        }

        double pass_lp_ms = passes[assigned_pass_idx].loop_point_ms;
        double pass_pad_ms = passes[assigned_pass_idx].arbitrary_pad_ms;
        double blank_space_ms = (patterns[p].start_ms - pass_lp_ms) + pass_pad_ms;
        if (blank_space_ms < 0.0) blank_space_ms = 0.0;

        uint64_t blank_frames = (uint64_t)round((blank_space_ms / 1000.0) * sr);

        char stem_wav_path[4096];
        snprintf(stem_wav_path, sizeof(stem_wav_path), "%s" PATH_SEP_STR "[stems]" PATH_SEP_STR "pattern_%d.wav", output_dir, p + 1);

        drwav wav_stem_out;
        if (drwav_init_file_write(&wav_stem_out, stem_wav_path, &format, NULL)) {
            int16_t chunk[8192];
            memset(chunk, 0, sizeof(chunk));

            // Write blank frames (silence)
            uint64_t remaining_blank = blank_frames;
            while (remaining_blank > 0) {
                uint64_t write_cnt = (remaining_blank < 8192) ? remaining_blank : 8192;
                drwav_write_pcm_frames(&wav_stem_out, write_cnt, chunk);
                remaining_blank -= write_cnt;
            }

            // Write pattern audio frames
            uint64_t remaining_audio = frame_count;
            uint64_t curr_smp = start_smp;
            while (remaining_audio > 0) {
                uint64_t write_cnt = (remaining_audio < 8192) ? remaining_audio : 8192;
                for (uint64_t i = 0; i < write_cnt; i++) {
                    float smp = y[curr_smp + i];
                    if (smp > 1.0f) smp = 1.0f;
                    if (smp < -1.0f) smp = -1.0f;
                    chunk[i] = (int16_t)(smp * 32767.0f);
                }
                drwav_write_pcm_frames(&wav_stem_out, write_cnt, chunk);
                curr_smp += write_cnt;
                remaining_audio -= write_cnt;
            }
            drwav_uninit(&wav_stem_out);
        }
    }

    // Export snapshots.bin and snapshots.js
    int total_peaks = 0;
    for (int b = 0; b < res2.num_bands; b++) total_peaks += res2.bands[b].num_peaks;

    char snap_bin_path[4096];
    snprintf(snap_bin_path, sizeof(snap_bin_path), "%s" PATH_SEP_STR "snapshots.bin", output_dir);
    FILE* f_snap = fopen(snap_bin_path, "wb");

    int snap_len = pass2_win_ms + 1;
    size_t float32_bytes_per_snap = snap_len * sizeof(float);
    size_t total_snap_bytes = total_peaks * float32_bytes_per_snap;
    uint8_t* all_snap_buf = (uint8_t*)malloc(total_snap_bytes ? total_snap_bytes : 1);

    size_t snap_offset = 0;
    for (int b = 0; b < res2.num_bands; b++) {
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
    snprintf(snap_js_path, sizeof(snap_js_path), "%s" PATH_SEP_STR "snapshots.js", output_dir);
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
    snprintf(manifest_path, sizeof(manifest_path), "%s" PATH_SEP_STR "manifest.json", output_dir);
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
        for (int b = 0; b < res2.num_bands; b++) {
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
    snprintf(r_data_json_path, sizeof(r_data_json_path), "%s" PATH_SEP_STR "report_data.json", output_dir);
    FILE* f_rd = fopen(r_data_json_path, "w");

    char r_data_js_path[4096];
    snprintf(r_data_js_path, sizeof(r_data_js_path), "%s" PATH_SEP_STR "report_data.js", output_dir);
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
        for (int b = 0; b < res2.num_bands; b++) {
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
            fprintf(fp, "  \"sample_rate\": %d,\n", sr);
            fprintf(fp, "  \"pass2_win_ms\": %d,\n", pass2_win_ms);
            fprintf(fp, "  \"frame_duration_ms\": %.6f,\n", 1000.0 * (double)((int)(sr * 0.001)) / (double)sr);
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

            // Accumulated Contour Histogram Data
            fprintf(fp, "  \"contour_histogram\": {\n");
            fprintf(fp, "    \"contour_len\": %d,\n", contour_len);
            fprintf(fp, "    \"sample_count\": %d,\n", contour_sample_count);
            fprintf(fp, "    \"high_point_ms\": %.2f,\n", best_bar_length_ms);
            fprintf(fp, "    \"contour\": [");
            for (int k = 0; k < contour_len; k++) {
                fprintf(fp, "%.4f%s", accumulated_contour ? accumulated_contour[k] : 0.0, (k < contour_len - 1) ? "," : "");
            }
            fprintf(fp, "]\n");
            fprintf(fp, "  },\n");

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
                for (int b = 0; b < res2.num_bands; b++) {
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

            // Midpoint Snapshot Data
            fprintf(fp, "  \"midpoint_snapshot\": {\n");
            fprintf(fp, "    \"midpoint_time_s\": %.4f,\n", midpoint_s);
            fprintf(fp, "    \"midpoint_time_ms\": %.2f,\n", midpoint_s * 1000.0);
            fprintf(fp, "    \"longest_hp_val_ms\": %.2f,\n", best_bar_length_ms);
            fprintf(fp, "    \"longest_hp_start_s\": %.4f,\n", longest_hp_start_s);
            fprintf(fp, "    \"longest_hp_end_s\": %.4f,\n", longest_hp_end_s);
            fprintf(fp, "    \"demarcation_line\": %.6f,\n", midpoint_demarcation_line);
            fprintf(fp, "    \"buffer_len\": %d,\n", pass1_window_ms + 1);
            fprintf(fp, "    \"buffer\": [");
            int mid_buf_len = pass1_window_ms + 1;
            for (int b_i = 0; b_i < mid_buf_len; b_i++) {
                fprintf(fp, "%.4f%s", midpoint_buffer ? midpoint_buffer[b_i] : 0.0, (b_i < mid_buf_len - 1) ? "," : "");
            }
            fprintf(fp, "]\n");
            fprintf(fp, "  },\n");

            // Flat peaks
            fprintf(fp, "  \"pass2_peaks\": [\n");
            int peak_iter = 0;
            size_t c_snap_offset = 0;
            for (int b = 0; b < res2.num_bands; b++) {
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
    if (midpoint_buffer) {
        free(midpoint_buffer);
        midpoint_buffer = NULL;
    }
    if (accumulated_contour) {
        free(accumulated_contour);
        accumulated_contour = NULL;
    }

    free(segments);

    // Export HTML Report File with full JS renderer functions
    char encoded_stem_name[2048];
    url_encode_filename(stem_name, encoded_stem_name, sizeof(encoded_stem_name));

    char html_filepath[4096];
    snprintf(html_filepath, sizeof(html_filepath), "%s" PATH_SEP_STR "%s_pattern_analysis.html", output_dir, stem_name);
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
        fprintf(f_html, "        #historyBufferCanvas { height: 250px; background-color: #ffffff; }\n");
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

        fprintf(f_html, "    <div class=\"section-title\">Accumulated Buffer Contour Histogram</div>\n");
        fprintf(f_html, "    <div class=\"canvas-container\" style=\"background: #ffffff;\">\n");
        fprintf(f_html, "        <canvas id=\"clusterHistogramCanvas\" width=\"1150\" height=\"220\"></canvas>\n");
        fprintf(f_html, "    </div>\n");
        fprintf(f_html, "    <div class=\"hint\">💡 Accumulated 15,000ms cumulative transience buffer contour recorded across the song (sampled every 19ms). The gold dashed line marks the segment length high point.</div>\n");

        fprintf(f_html, "    <div class=\"section-title\">Cumulative History Buffer at Longest High Point Midpoint</div>\n");
        fprintf(f_html, "    <div class=\"canvas-container\" style=\"background: #ffffff;\">\n");
        fprintf(f_html, "        <canvas id=\"midpointBufferCanvas\" width=\"1150\" height=\"250\"></canvas>\n");
        fprintf(f_html, "    </div>\n");
        fprintf(f_html, "    <div class=\"hint\">💡 State of the 15,000ms cumulative history buffer captured at the midpoint during the duration of the longest stable high point (determined on Pass 1). The red dashed line denotes the high point bar duration.</div>\n");

        fprintf(f_html, "    <div class=\"section-title\">1. Interactive Audio Waveform, Pattern Map & Cumulative History High Point Changes</div>\n");
        fprintf(f_html, "    <div class=\"audio-controls\">\n");
        fprintf(f_html, "        <audio id=\"audioPlayer\" controls src=\"%s.wav\"></audio>\n", encoded_stem_name);
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
        fprintf(f_html, "        <canvas id=\"historyBufferCanvas\" width=\"1150\" height=\"250\"></canvas>\n");
        fprintf(f_html, "    </div>\n");
        fprintf(f_html, "    <div class=\"hint\">💡 Select a section to zoom in on. Right-click anywhere on the graph to zoom back out to the full duration. Graph updates in real time during audio playback.</div>\n");
        fprintf(f_html, "</div>\n");

        fprintf(f_html, "<script>\n");
        fprintf(f_html, "    let sampleRate = %d;\n", sr);
        fprintf(f_html, "    let pass2WinMs = %d;\n", pass2_win_ms);
        fprintf(f_html, "    let frameDurationMs = 1000.0 * Math.floor(%d * 0.001) / %d;\n", sr, sr);
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
        fprintf(f_html, "    let pass2Peaks = [];\n");
        fprintf(f_html, "    let midpointSnapshot = null;\n");
        fprintf(f_html, "    let contourHistogram = null;\n\n");

        fprintf(f_html, "    const canvas = document.getElementById('waveformCanvas');\n");
        fprintf(f_html, "    const ctx = canvas ? canvas.getContext('2d') : null;\n");
        fprintf(f_html, "    const audio = document.getElementById('audioPlayer');\n");
        fprintf(f_html, "    const bufCanvas = document.getElementById('historyBufferCanvas');\n");
        fprintf(f_html, "    const bufCtx = bufCanvas ? bufCanvas.getContext('2d') : null;\n");
        fprintf(f_html, "    const midCanvas = document.getElementById('midpointBufferCanvas');\n");
        fprintf(f_html, "    const midCtx = midCanvas ? midCanvas.getContext('2d') : null;\n");
        fprintf(f_html, "    const histCanvas = document.getElementById('clusterHistogramCanvas');\n");
        fprintf(f_html, "    const histCtx = histCanvas ? histCanvas.getContext('2d') : null;\n\n");

        fprintf(f_html, "    let zoomStartMs = -pass2WinMs;\n");
        fprintf(f_html, "    let zoomEndMs = 0.0;\n");
        fprintf(f_html, "    let isDraggingBuf = false;\n");
        fprintf(f_html, "    let dragStartX = 0, dragCurrentX = 0;\n\n");


        fprintf(f_html, "    const colors = ['rgba(46, 204, 113, 0.35)', 'rgba(231, 76, 60, 0.35)', 'rgba(155, 89, 182, 0.35)', 'rgba(241, 196, 15, 0.35)'];\n");
        fprintf(f_html, "    const borderColors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f'];\n\n");

        fprintf(f_html, "    function formatMSS(sec) {\n");
        fprintf(f_html, "        if (isNaN(sec) || sec < 0) sec = 0;\n");
        fprintf(f_html, "        const m = Math.floor(sec / 60);\n");
        fprintf(f_html, "        const s = Math.floor(sec %% 60);\n");
        fprintf(f_html, "        return `${m}:${s < 10 ? '0' : ''}${s}`;\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function applyReportData(data) {\n");
        fprintf(f_html, "        if (!data) return;\n");
        fprintf(f_html, "        if (data.sample_rate !== undefined) { sampleRate = data.sample_rate; frameDurationMs = 1000.0 * Math.floor(sampleRate * 0.001) / sampleRate; }\n");
        fprintf(f_html, "        if (data.pass2_win_ms !== undefined) { pass2WinMs = data.pass2_win_ms; zoomStartMs = -pass2WinMs; }\n");
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
        fprintf(f_html, "        if (data.midpoint_snapshot) midpointSnapshot = data.midpoint_snapshot;\n");
        fprintf(f_html, "        if (data.contour_histogram) contourHistogram = data.contour_histogram;\n");
        fprintf(f_html, "        renderAll();\n");
        fprintf(f_html, "    }\n\n");

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

        fprintf(f_html, "        const centerY = (H - 25) / 2;\n");
        fprintf(f_html, "        const numPts = waveformMin.length;\n");
        fprintf(f_html, "        ctx.lineWidth = 1.2; ctx.strokeStyle = 'rgba(44, 62, 80, 0.45)'; ctx.beginPath();\n");
        fprintf(f_html, "        for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "            const x = (i / numPts) * W;\n");
        fprintf(f_html, "            const minY = centerY - (waveformMin[i] * ((H - 25) * 0.42));\n");
        fprintf(f_html, "            const maxY = centerY - (waveformMax[i] * ((H - 25) * 0.42));\n");
        fprintf(f_html, "            ctx.moveTo(x, minY); ctx.lineTo(x, maxY);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        ctx.stroke();\n");

        fprintf(f_html, "        patterns.forEach((pat, idx) => {\n");
        fprintf(f_html, "            const color = colors[idx %% colors.length];\n");
        fprintf(f_html, "            const borderColor = borderColors[idx %% borderColors.length];\n");
        fprintf(f_html, "            const startX = (pat.start_ms / 1000.0 / totalDurationS) * W;\n");
        fprintf(f_html, "            const endX = (pat.end_ms / 1000.0 / totalDurationS) * W;\n");
        fprintf(f_html, "            ctx.fillStyle = color; ctx.fillRect(startX, 0, endX - startX, H - 25);\n");
        fprintf(f_html, "            ctx.fillStyle = borderColor; ctx.font = 'bold 12px Segoe UI, sans-serif'; ctx.textAlign = 'left';\n");
        fprintf(f_html, "            ctx.fillText(`Pattern ${idx + 1} (${Math.round(pat.duration_ms)}ms)`, startX + 5, 20);\n");
        fprintf(f_html, "            if (pat.segments) {\n");
        fprintf(f_html, "                pat.segments.forEach(segIdx => {\n");
        fprintf(f_html, "                    const segStartX = (segIdx * bestBarLengthMs / 1000.0 / totalDurationS) * W;\n");
        fprintf(f_html, "                    ctx.strokeStyle = borderColor; ctx.setLineDash([4, 4]);\n");
        fprintf(f_html, "                    ctx.beginPath(); ctx.moveTo(segStartX, 0); ctx.lineTo(segStartX, H - 25); ctx.stroke(); ctx.setLineDash([]);\n");
        fprintf(f_html, "                    ctx.fillStyle = borderColor; ctx.font = '10px Segoe UI, sans-serif';\n");
        fprintf(f_html, "                    ctx.fillText(`Seg ${segIdx}`, segStartX + 3, 38);\n");
        fprintf(f_html, "                });\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        });\n");

        fprintf(f_html, "        ctx.strokeStyle = '#7f8c8d'; ctx.lineWidth = 1;\n");
        fprintf(f_html, "        ctx.beginPath(); ctx.moveTo(0, H - 25); ctx.lineTo(W, H - 25); ctx.stroke();\n");
        fprintf(f_html, "        ctx.fillStyle = '#7f8c8d'; ctx.font = '11px Segoe UI, sans-serif'; ctx.textAlign = 'center';\n");
        fprintf(f_html, "        const numWfTicks = 10;\n");
        fprintf(f_html, "        for (let i = 0; i <= numWfTicks; i++) {\n");
        fprintf(f_html, "            const frac = i / numWfTicks;\n");
        fprintf(f_html, "            const x = frac * W;\n");
        fprintf(f_html, "            const secVal = frac * totalDurationS;\n");
        fprintf(f_html, "            ctx.beginPath(); ctx.moveTo(x, H - 25); ctx.lineTo(x, H - 20); ctx.stroke();\n");
        fprintf(f_html, "            ctx.fillText(formatMSS(secVal), x, H - 8);\n");
        fprintf(f_html, "        }\n");

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
        fprintf(f_html, "            if (c) { delete c.dataset.startMs; delete c.dataset.endMs; }\n");
        fprintf(f_html, "            sCtx.fillStyle = '#f8f9fa'; sCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "            return;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (c) {\n");
        fprintf(f_html, "            c.dataset.startMs = seg.start_ms;\n");
        fprintf(f_html, "            c.dataset.endMs = seg.end_ms;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const patIdx = patterns.findIndex(p => p.segments && p.segments.includes(seg.segment_index));\n");
        fprintf(f_html, "        if (patIdx !== -1) {\n");
        fprintf(f_html, "            const mainColor = borderColors[patIdx %% borderColors.length];\n");
        fprintf(f_html, "            if (boxElem) { boxElem.style.borderColor = mainColor; boxElem.style.borderWidth = '2px'; }\n");
        fprintf(f_html, "            if (t) t.textContent = `${labelPrefix} Segment (Seg ${seg.segment_index} [Pattern ${patIdx + 1}] : ${Math.round(seg.start_ms)} - ${Math.round(seg.end_ms)} ms)`;\n");
        fprintf(f_html, "        } else {\n");
        fprintf(f_html, "            if (boxElem) { boxElem.style.borderColor = ''; boxElem.style.borderWidth = ''; }\n");
        fprintf(f_html, "            if (t) t.textContent = `${labelPrefix} Segment (Seg ${seg.segment_index} : ${Math.round(seg.start_ms)} - ${Math.round(seg.end_ms)} ms)`;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (r) r.textContent = `Average Rating: ${seg.rating.toFixed(4)}`;\n");
        fprintf(f_html, "        sCtx.fillStyle = '#ffffff'; sCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "        const padL = 0, padR = 0, padT = 10, padB = 22;\n");
        fprintf(f_html, "        const graphW = W - padL - padR, graphH = H - padT - padB;\n");
        fprintf(f_html, "        const segDurMs = seg.end_ms - seg.start_ms;\n");

        fprintf(f_html, "        sCtx.strokeStyle = '#dcdde1'; sCtx.lineWidth = 1; sCtx.strokeRect(padL, padT, graphW, graphH);\n");
        fprintf(f_html, "        const centerY = padT + graphH / 2;\n");
        fprintf(f_html, "        const latestActivePeak = getLatestActivePeak();\n");

        fprintf(f_html, "        if (seg.peaks && seg.peaks.length > 0) {\n");
        fprintf(f_html, "            seg.peaks.forEach(p => {\n");
        fprintf(f_html, "                const relMs = p.time_ms - seg.start_ms;\n");
        fprintf(f_html, "                const x = padL + (relMs / segDurMs) * graphW;\n");
        fprintf(f_html, "                const isLatest = latestActivePeak && (latestActivePeak.p_idx === p.p_idx);\n");
        fprintf(f_html, "                sCtx.strokeStyle = getScoreColor(p.total_score, isLatest ? 0.9 : 0.55);\n");
        fprintf(f_html, "                sCtx.lineWidth = isLatest ? 2.5 : 1.8; sCtx.setLineDash([4, 4]);\n");
        fprintf(f_html, "                sCtx.beginPath(); sCtx.moveTo(x, padT); sCtx.lineTo(x, padT + graphH); sCtx.stroke(); sCtx.setLineDash([]);\n");
        fprintf(f_html, "                let scoreY = centerY;\n");
        fprintf(f_html, "                if (p.total_score > 0) {\n");
        fprintf(f_html, "                    const frac = globalMaxPosScore > 0 ? Math.min(1.0, Math.max(0.0, p.total_score / globalMaxPosScore)) : 0.0;\n");
        fprintf(f_html, "                    scoreY = centerY - frac * (centerY - padT);\n");
        fprintf(f_html, "                } else if (p.total_score < 0) {\n");
        fprintf(f_html, "                    const frac = globalMinNegScore < 0 ? Math.min(1.0, Math.max(0.0, p.total_score / globalMinNegScore)) : 0.0;\n");
        fprintf(f_html, "                    scoreY = centerY + frac * (padT + graphH - centerY);\n");
        fprintf(f_html, "                }\n");
        fprintf(f_html, "                scoreY = Math.max(padT + 10, Math.min(padT + graphH - 4, scoreY));\n");
        fprintf(f_html, "                sCtx.fillStyle = getScoreColor(p.total_score, 1.0);\n");
        fprintf(f_html, "                sCtx.font = isLatest ? 'bold 11px Segoe UI, sans-serif' : '10px Segoe UI, sans-serif';\n");
        fprintf(f_html, "                sCtx.textAlign = (x > padL + graphW - 40) ? 'right' : 'left';\n");
        fprintf(f_html, "                const textX = (x > padL + graphW - 40) ? x - 4 : x + 4;\n");
        fprintf(f_html, "                sCtx.fillText(`${p.total_score >= 0 ? '+' : ''}${p.total_score.toFixed(2)}`, textX, scoreY);\n");
        fprintf(f_html, "            });\n");
        fprintf(f_html, "        }\n");

        fprintf(f_html, "        sCtx.strokeStyle = 'rgba(189, 195, 199, 0.4)'; sCtx.beginPath(); sCtx.moveTo(padL, centerY); sCtx.lineTo(padL + graphW, centerY); sCtx.stroke();\n");

        fprintf(f_html, "        if (seg.waveform_min && seg.waveform_min.length > 0) {\n");
        fprintf(f_html, "            const numPts = seg.waveform_min.length;\n");
        fprintf(f_html, "            sCtx.lineWidth = 1.0; sCtx.strokeStyle = '#34495e'; sCtx.beginPath();\n");
        fprintf(f_html, "            for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "                const x = padL + (i / numPts) * graphW;\n");
        fprintf(f_html, "                const minY = centerY - (seg.waveform_min[i] * (graphH * 0.42));\n");
        fprintf(f_html, "                const maxY = centerY - (seg.waveform_max[i] * (graphH * 0.42));\n");
        fprintf(f_html, "                sCtx.moveTo(x, minY); sCtx.lineTo(x, maxY);\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "            sCtx.stroke();\n");
        fprintf(f_html, "        }\n");

        fprintf(f_html, "        sCtx.fillStyle = '#7f8c8d'; sCtx.font = '9px Segoe UI, sans-serif';\n");
        fprintf(f_html, "        const numSegTicks = 5;\n");
        fprintf(f_html, "        for (let i = 0; i <= numSegTicks; i++) {\n");
        fprintf(f_html, "            const frac = i / numSegTicks;\n");
        fprintf(f_html, "            const x = padL + frac * graphW;\n");
        fprintf(f_html, "            const timeS = (seg.start_ms + frac * segDurMs) / 1000.0;\n");
        fprintf(f_html, "            sCtx.beginPath(); sCtx.moveTo(x, padT + graphH); sCtx.lineTo(x, padT + graphH + 3); sCtx.stroke();\n");
        fprintf(f_html, "            if (i === 0) sCtx.textAlign = 'left';\n");
        fprintf(f_html, "            else if (i === numSegTicks) sCtx.textAlign = 'right';\n");
        fprintf(f_html, "            else sCtx.textAlign = 'center';\n");
        fprintf(f_html, "            const textX = (i === 0) ? x + 2 : (i === numSegTicks ? x - 2 : x);\n");
        fprintf(f_html, "            sCtx.fillText(formatMSS(timeS), textX, H - 2);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (currentAudioTimeMs >= seg.start_ms && currentAudioTimeMs <= seg.end_ms) {\n");
        fprintf(f_html, "            const cursorX = padL + ((currentAudioTimeMs - seg.start_ms) / segDurMs) * graphW;\n");
        fprintf(f_html, "            sCtx.strokeStyle = '#e67e22'; sCtx.lineWidth = 2.5; sCtx.beginPath(); sCtx.moveTo(cursorX, padT); sCtx.lineTo(cursorX, padT + graphH); sCtx.stroke();\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function drawClusterHistogram() {\n");
        fprintf(f_html, "        if (!histCanvas || !histCtx) return;\n");
        fprintf(f_html, "        const W = histCanvas.width, H = histCanvas.height;\n");
        fprintf(f_html, "        histCtx.clearRect(0, 0, W, H);\n");
        fprintf(f_html, "        histCtx.fillStyle = '#ffffff'; histCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "        if (!contourHistogram || !contourHistogram.contour || contourHistogram.contour.length === 0) {\n");
        fprintf(f_html, "            histCtx.fillStyle = '#7f8c8d'; histCtx.font = '13px Segoe UI, sans-serif'; histCtx.textAlign = 'center';\n");
        fprintf(f_html, "            histCtx.fillText('No accumulated buffer contour histogram data available.', W / 2, H / 2);\n");
        fprintf(f_html, "            return;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const contour = contourHistogram.contour;\n");
        fprintf(f_html, "        const numPts = contour.length;\n");
        fprintf(f_html, "        const maxX = numPts - 1;\n");
        fprintf(f_html, "        const padLeft = 70, padRight = 35, padTop = 35, padBottom = 45;\n");
        fprintf(f_html, "        const graphW = W - padLeft - padRight, graphH = H - padTop - padBottom;\n");
        fprintf(f_html, "        histCtx.strokeStyle = '#dcdde1'; histCtx.lineWidth = 1; histCtx.strokeRect(padLeft, padTop, graphW, graphH);\n");

        fprintf(f_html, "        const mLen = numPts - 99;\n");
        fprintf(f_html, "        let maxVal = 0.0;\n");
        fprintf(f_html, "        for (let i = 0; i < (mLen > 0 ? mLen : numPts); i++) {\n");
        fprintf(f_html, "            if (contour[i] > maxVal) maxVal = contour[i];\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (maxVal <= 0) maxVal = 1.0;\n");

        fprintf(f_html, "        histCtx.fillStyle = 'rgba(46, 204, 113, 0.15)';\n");
        fprintf(f_html, "        histCtx.beginPath();\n");
        fprintf(f_html, "        histCtx.moveTo(padLeft, padTop + graphH);\n");
        fprintf(f_html, "        for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "            const x = padLeft + (i / maxX) * graphW;\n");
        fprintf(f_html, "            const y = padTop + graphH - (contour[i] / maxVal) * (graphH * 0.88);\n");
        fprintf(f_html, "            histCtx.lineTo(x, y);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        histCtx.lineTo(padLeft + graphW, padTop + graphH);\n");
        fprintf(f_html, "        histCtx.closePath();\n");
        fprintf(f_html, "        histCtx.fill();\n");

        fprintf(f_html, "        histCtx.strokeStyle = '#27ae60'; histCtx.lineWidth = 2.5; histCtx.beginPath();\n");
        fprintf(f_html, "        for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "            const x = padLeft + (i / maxX) * graphW;\n");
        fprintf(f_html, "            const y = padTop + graphH - (contour[i] / maxVal) * (graphH * 0.88);\n");
        fprintf(f_html, "            if (i === 0) histCtx.moveTo(x, y); else histCtx.lineTo(x, y);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        histCtx.stroke();\n");

        fprintf(f_html, "        const centroidMs = contourHistogram.high_point_ms || bestBarLengthMs;\n");
        fprintf(f_html, "        const lineIdx = maxX - centroidMs;\n");
        fprintf(f_html, "        const cx = padLeft + (lineIdx / maxX) * graphW;\n");
        fprintf(f_html, "        if (cx >= padLeft && cx <= padLeft + graphW) {\n");
        fprintf(f_html, "            histCtx.strokeStyle = '#f39c12'; histCtx.lineWidth = 2.5; histCtx.setLineDash([4, 4]);\n");
        fprintf(f_html, "            histCtx.beginPath(); histCtx.moveTo(cx, padTop); histCtx.lineTo(cx, padTop + graphH); histCtx.stroke(); histCtx.setLineDash([]);\n");
        fprintf(f_html, "            histCtx.fillStyle = '#d35400'; histCtx.font = 'bold 12px Segoe UI, sans-serif';\n");
        fprintf(f_html, "            histCtx.textAlign = (cx > padLeft + graphW - 160) ? 'right' : 'left';\n");
        fprintf(f_html, "            const tx = (cx > padLeft + graphW - 160) ? cx - 6 : cx + 6;\n");
        fprintf(f_html, "            histCtx.fillText(`Segment Length High Point: -${centroidMs.toFixed(2)} ms (${centroidMs.toFixed(2)} ms)`, tx, padTop + 18);\n");
        fprintf(f_html, "        }\n");

        fprintf(f_html, "        histCtx.fillStyle = '#7f8c8d'; histCtx.font = '11px Segoe UI, sans-serif'; histCtx.textAlign = 'center';\n");
        fprintf(f_html, "        for (let i = 0; i <= 5; i++) {\n");
        fprintf(f_html, "            const frac = i / 5;\n");
        fprintf(f_html, "            const x = padLeft + frac * graphW;\n");
        fprintf(f_html, "            const msVal = (frac - 1.0) * maxX;\n");
        fprintf(f_html, "            histCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)'; histCtx.beginPath(); histCtx.moveTo(x, padTop); histCtx.lineTo(x, padTop + graphH); histCtx.stroke();\n");
        fprintf(f_html, "            histCtx.fillText((Math.abs(msVal) < 1e-3) ? '0 ms' : `${Math.round(msVal)} ms`, x, padTop + graphH + 18);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        histCtx.fillStyle = '#2c3e50'; histCtx.font = 'bold 13px Segoe UI, sans-serif'; histCtx.textAlign = 'left';\n");
        fprintf(f_html, "        histCtx.fillText(`Accumulated Buffer Contour Histogram (Segment Length: ${bestBarLengthMs.toFixed(2)} ms)`, padLeft, padTop - 10);\n");
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

        fprintf(f_html, "    function drawMidpointBuffer() {\n");
        fprintf(f_html, "        if (!midCanvas || !midCtx) return;\n");
        fprintf(f_html, "        const W = midCanvas.width, H = midCanvas.height;\n");
        fprintf(f_html, "        midCtx.clearRect(0, 0, W, H);\n");
        fprintf(f_html, "        midCtx.fillStyle = '#ffffff'; midCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "        if (!midpointSnapshot || !midpointSnapshot.buffer) {\n");
        fprintf(f_html, "            midCtx.fillStyle = '#7f8c8d'; midCtx.font = '13px Segoe UI, sans-serif'; midCtx.textAlign = 'center';\n");
        fprintf(f_html, "            midCtx.fillText('No midpoint history buffer data available.', W / 2, H / 2);\n");
        fprintf(f_html, "            return;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const buf = midpointSnapshot.buffer;\n");
        fprintf(f_html, "        const numPts = buf.length;\n");
        fprintf(f_html, "        const pass1WinMs = 15000.0;\n");
        fprintf(f_html, "        const snapBinMs = pass1WinMs / (numPts - 1 || 1);\n");
        fprintf(f_html, "        const winStartMs = -pass1WinMs, winEndMs = 0.0;\n");
        fprintf(f_html, "        let curMax = 0;\n");
        fprintf(f_html, "        for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "            const sampleMs = (i - (numPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "            if (sampleMs <= -99.0 + 1e-5 && buf[i] > curMax) curMax = buf[i];\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (curMax <= 0) curMax = 1.0;\n");
        fprintf(f_html, "        const yMax = curMax * 1.1;\n");
        fprintf(f_html, "        const padLeft = 70, padRight = 35, padTop = 35, padBottom = 45;\n");
        fprintf(f_html, "        const graphW = W - padLeft - padRight, graphH = H - padTop - padBottom;\n");
        fprintf(f_html, "        midCtx.strokeStyle = '#dcdde1'; midCtx.lineWidth = 1; midCtx.strokeRect(padLeft, padTop, graphW, graphH);\n");
        fprintf(f_html, "        midCtx.fillStyle = '#7f8c8d'; midCtx.font = '11px Segoe UI, sans-serif'; midCtx.textAlign = 'center';\n");
        fprintf(f_html, "        for (let i = 0; i <= 5; i++) {\n");
        fprintf(f_html, "            const frac = i / 5;\n");
        fprintf(f_html, "            const x = padLeft + frac * graphW;\n");
        fprintf(f_html, "            const msVal = winStartMs + frac * pass1WinMs;\n");
        fprintf(f_html, "            midCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)'; midCtx.beginPath(); midCtx.moveTo(x, padTop); midCtx.lineTo(x, padTop + graphH); midCtx.stroke();\n");
        fprintf(f_html, "            midCtx.fillText((Math.abs(msVal) < 1e-3) ? '0ms' : `${Math.round(msVal)}ms`, x, padTop + graphH + 18);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        midCtx.textAlign = 'right';\n");
        fprintf(f_html, "        for (let ratio of [0.0, 0.25, 0.5, 0.75, 1.0]) {\n");
        fprintf(f_html, "            const y = padTop + graphH - ratio * graphH;\n");
        fprintf(f_html, "            midCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)'; midCtx.beginPath(); midCtx.moveTo(padLeft, y); midCtx.lineTo(padLeft + graphW, y); midCtx.stroke();\n");
        fprintf(f_html, "            midCtx.fillText((yMax * ratio).toFixed(2), padLeft - 8, y + 4);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const meanVal = midpointSnapshot.demarcation_line || 0;\n");
        fprintf(f_html, "        const meanY = padTop + graphH - (meanVal / yMax) * graphH;\n");
        fprintf(f_html, "        midCtx.strokeStyle = '#7f8c8d'; midCtx.lineWidth = 1.2; midCtx.setLineDash([5, 5]);\n");
        fprintf(f_html, "        midCtx.beginPath(); midCtx.moveTo(padLeft, meanY); midCtx.lineTo(padLeft + graphW, meanY); midCtx.stroke(); midCtx.setLineDash([]);\n");
        fprintf(f_html, "        midCtx.save(); midCtx.beginPath(); midCtx.rect(padLeft, padTop, graphW, graphH); midCtx.clip();\n");
        fprintf(f_html, "        midCtx.strokeStyle = '#3498db'; midCtx.lineWidth = 2.0; midCtx.fillStyle = 'rgba(52, 152, 219, 0.15)';\n");
        fprintf(f_html, "        midCtx.beginPath(); let firstPt = true; let lastDrawnMs = (0 - (numPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "        for (let i = 0; i < numPts; i++) {\n");
        fprintf(f_html, "            const sampleMs = (i - (numPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "            if (sampleMs > -99.0 + 1e-5) continue;\n");
        fprintf(f_html, "            const x = padLeft + ((sampleMs - winStartMs) / pass1WinMs) * graphW;\n");
        fprintf(f_html, "            const y = padTop + graphH - (buf[i] / yMax) * graphH;\n");
        fprintf(f_html, "            if (firstPt) { midCtx.moveTo(x, y); firstPt = false; } else { midCtx.lineTo(x, y); }\n");
        fprintf(f_html, "            lastDrawnMs = sampleMs;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        midCtx.stroke();\n");
        fprintf(f_html, "        const endX = padLeft + ((lastDrawnMs - winStartMs) / pass1WinMs) * graphW;\n");
        fprintf(f_html, "        const startX = padLeft + (((0 - (numPts - 1)) * snapBinMs - winStartMs) / pass1WinMs) * graphW;\n");
        fprintf(f_html, "        midCtx.lineTo(endX, padTop + graphH); midCtx.lineTo(startX, padTop + graphH); midCtx.closePath(); midCtx.fill();\n");

        fprintf(f_html, "        const hpVal = midpointSnapshot.longest_hp_val_ms || bestBarLengthMs;\n");
        fprintf(f_html, "        if (hpVal > 0) {\n");
        fprintf(f_html, "            const hpX = padLeft + (((-hpVal) - winStartMs) / pass1WinMs) * graphW;\n");
        fprintf(f_html, "            if (hpX >= padLeft && hpX <= padLeft + graphW) {\n");
        fprintf(f_html, "                midCtx.strokeStyle = '#e74c3c'; midCtx.lineWidth = 2.0; midCtx.setLineDash([4, 4]);\n");
        fprintf(f_html, "                midCtx.beginPath(); midCtx.moveTo(hpX, padTop); midCtx.lineTo(hpX, padTop + graphH); midCtx.stroke(); midCtx.setLineDash([]);\n");
        fprintf(f_html, "                midCtx.fillStyle = '#e74c3c'; midCtx.font = 'bold 11px Segoe UI, sans-serif';\n");
        fprintf(f_html, "                midCtx.textAlign = (hpX > padLeft + graphW - 90) ? 'right' : 'left';\n");
        fprintf(f_html, "                const textX = (hpX > padLeft + graphW - 90) ? hpX - 5 : hpX + 5;\n");
        fprintf(f_html, "                midCtx.fillText(`High Point: ${Math.round(hpVal)}ms`, textX, padTop + 15);\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        }\n");

        fprintf(f_html, "        midCtx.restore();\n");
        fprintf(f_html, "        midCtx.save(); midCtx.translate(20, padTop + graphH / 2); midCtx.rotate(-Math.PI / 2);\n");
        fprintf(f_html, "        midCtx.fillStyle = '#7f8c8d'; midCtx.font = '11px Segoe UI, sans-serif'; midCtx.textAlign = 'center';\n");
        fprintf(f_html, "        midCtx.fillText('Transience Flux / Energy', 0, 0); midCtx.restore();\n");
        fprintf(f_html, "        midCtx.fillStyle = '#7f8c8d'; midCtx.font = '11px Segoe UI, sans-serif'; midCtx.textAlign = 'center';\n");
        fprintf(f_html, "        midCtx.fillText('Historical Window Time (ms)', padLeft + graphW / 2, padTop + graphH + 34);\n");
        fprintf(f_html, "        const midTimeMs = midpointSnapshot.midpoint_time_ms || (midpointSnapshot.midpoint_time_s * 1000.0) || 0;\n");
        fprintf(f_html, "        const midTimeStr = formatMSS(midTimeMs / 1000.0);\n");
        fprintf(f_html, "        midCtx.fillStyle = '#2c3e50'; midCtx.font = 'bold 13px Segoe UI, sans-serif'; midCtx.textAlign = 'left';\n");
        fprintf(f_html, "        midCtx.fillText(`15000ms Cumulative Buffer at Longest High Point Midpoint (${midTimeStr} / ${Math.round(midTimeMs)}ms - Bar: ${Math.round(hpVal)}ms)`, padLeft, padTop - 10);\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function drawHistoryBuffer() {\n");
        fprintf(f_html, "        if (!bufCanvas || !bufCtx) return;\n");
        fprintf(f_html, "        const W = bufCanvas.width, H = bufCanvas.height;\n");
        fprintf(f_html, "        bufCtx.clearRect(0, 0, W, H);\n");
        fprintf(f_html, "        bufCtx.fillStyle = '#ffffff'; bufCtx.fillRect(0, 0, W, H);\n");
        fprintf(f_html, "        const curTimeMs = (audio.currentTime || 0) * 1000.0;\n");
        fprintf(f_html, "        const padLeft = 70, padRight = 35, padTop = 35, padBottom = 45;\n");
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
        fprintf(f_html, "        const numTicks = 5;\n");
        fprintf(f_html, "        bufCtx.fillStyle = '#7f8c8d'; bufCtx.font = '11px Segoe UI, sans-serif'; bufCtx.textAlign = 'center';\n");
        fprintf(f_html, "        for (let i = 0; i <= numTicks; i++) {\n");
        fprintf(f_html, "            const frac = i / numTicks;\n");
        fprintf(f_html, "            const x = padLeft + frac * graphW;\n");
        fprintf(f_html, "            const msVal = zoomStartMs + frac * zoomSpan;\n");
        fprintf(f_html, "            bufCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)'; bufCtx.beginPath(); bufCtx.moveTo(x, padTop); bufCtx.lineTo(x, padTop + graphH); bufCtx.stroke();\n");
        fprintf(f_html, "            bufCtx.fillText((Math.abs(msVal) < 1e-3) ? '0ms' : `${Math.round(msVal)}ms`, x, padTop + graphH + 18);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        bufCtx.textAlign = 'right';\n");
        fprintf(f_html, "        for (let ratio of [0.0, 0.25, 0.5, 0.75, 1.0]) {\n");
        fprintf(f_html, "            const y = padTop + graphH - ratio * graphH;\n");
        fprintf(f_html, "            bufCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)'; bufCtx.beginPath(); bufCtx.moveTo(padLeft, y); bufCtx.lineTo(padLeft + graphW, y); bufCtx.stroke();\n");
        fprintf(f_html, "            bufCtx.fillText((yMax * ratio).toFixed(2), padLeft - 8, y + 4);\n");
        fprintf(f_html, "        }\n");

        fprintf(f_html, "        const meanVal = (latestPeak && latestPeak.demarcation_line !== undefined) ? latestPeak.demarcation_line : 0;\n");
        fprintf(f_html, "        const meanY = padTop + graphH - (meanVal / yMax) * graphH;\n");
        fprintf(f_html, "        bufCtx.strokeStyle = '#7f8c8d'; bufCtx.lineWidth = 1.2; bufCtx.setLineDash([5, 5]);\n");
        fprintf(f_html, "        bufCtx.beginPath(); bufCtx.moveTo(padLeft, meanY); bufCtx.lineTo(padLeft + graphW, meanY); bufCtx.stroke(); bufCtx.setLineDash([]);\n");

        fprintf(f_html, "        bufCtx.save(); bufCtx.beginPath(); bufCtx.rect(padLeft, padTop, graphW, graphH); bufCtx.clip();\n");
        fprintf(f_html, "        bufCtx.strokeStyle = '#3498db'; bufCtx.lineWidth = 2.0; bufCtx.fillStyle = 'rgba(52, 152, 219, 0.15)';\n");
        fprintf(f_html, "        bufCtx.beginPath(); let firstPt = true; let lastDrawnMs = (0 - (numBufPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "        for (let i = 0; i < numBufPts; i++) {\n");
        fprintf(f_html, "            const sampleMs = (i - (numBufPts - 1)) * snapBinMs;\n");
        fprintf(f_html, "            if (sampleMs > -99.0 + 1e-5) continue;\n");
        fprintf(f_html, "            const x = padLeft + ((sampleMs - zoomStartMs) / zoomSpan) * graphW;\n");
        fprintf(f_html, "            const y = padTop + graphH - (accumulatedBuffer[i] / yMax) * graphH;\n");
        fprintf(f_html, "            if (firstPt) { bufCtx.moveTo(x, y); firstPt = false; } else { bufCtx.lineTo(x, y); }\n");
        fprintf(f_html, "            lastDrawnMs = sampleMs;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        bufCtx.stroke();\n");
        fprintf(f_html, "        const endX = padLeft + ((lastDrawnMs - zoomStartMs) / zoomSpan) * graphW;\n");
        fprintf(f_html, "        const startX = padLeft + (((0 - (numBufPts - 1)) * snapBinMs - zoomStartMs) / zoomSpan) * graphW;\n");
        fprintf(f_html, "        bufCtx.lineTo(endX, padTop + graphH); bufCtx.lineTo(startX, padTop + graphH); bufCtx.closePath(); bufCtx.fill();\n");

        fprintf(f_html, "        if (activePeaks.length > 0) {\n");
        fprintf(f_html, "            const lPeak = activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b));\n");
        fprintf(f_html, "            if (lPeak && lPeak.qualifiers) {\n");
        fprintf(f_html, "                lPeak.qualifiers.forEach(q => {\n");
        fprintf(f_html, "                    const qMs = q.ms, qVal = q.val, qOrigMs = (q.orig_ms !== undefined) ? q.orig_ms : q.ms;\n");
        fprintf(f_html, "                    const qx = padLeft + ((qMs - zoomStartMs) / zoomSpan) * graphW;\n");
        fprintf(f_html, "                    const qOrigX = padLeft + ((qOrigMs - zoomStartMs) / zoomSpan) * graphW;\n");
        fprintf(f_html, "                    const tolW = (toleranceMs / zoomSpan) * graphW;\n");
        fprintf(f_html, "                    const scoreColor = getQualifierColor(qVal, 0.85);\n");
        fprintf(f_html, "                    const spanColor = getQualifierColor(qVal, 0.18);\n");
        fprintf(f_html, "                    const spanX1 = Math.max(padLeft, qOrigX - tolW);\n");
        fprintf(f_html, "                    const spanX2 = Math.min(padLeft + graphW, qOrigX + tolW);\n");
        fprintf(f_html, "                    if (spanX2 > spanX1) { bufCtx.fillStyle = spanColor; bufCtx.fillRect(spanX1, padTop, spanX2 - spanX1, graphH); }\n");
        fprintf(f_html, "                    if (qx >= padLeft && qx <= padLeft + graphW) {\n");
        fprintf(f_html, "                        bufCtx.strokeStyle = scoreColor; bufCtx.lineWidth = 2.0; bufCtx.setLineDash([3, 3]);\n");
        fprintf(f_html, "                        bufCtx.beginPath(); bufCtx.moveTo(qx, padTop); bufCtx.lineTo(qx, padTop + graphH); bufCtx.stroke(); bufCtx.setLineDash([]);\n");
        fprintf(f_html, "                        let scoreY = meanY;\n");
        fprintf(f_html, "                        if (qVal > 0) scoreY = meanY - (Math.min(1.0, qVal / 1.0)) * (meanY - padTop);\n");
        fprintf(f_html, "                        else if (qVal < 0) scoreY = meanY + (Math.min(1.0, Math.abs(qVal) / 1.0)) * (padTop + graphH - meanY);\n");
        fprintf(f_html, "                        bufCtx.fillStyle = scoreColor; bufCtx.font = 'bold 11px Segoe UI, sans-serif';\n");
        fprintf(f_html, "                        bufCtx.textAlign = (qx > padLeft + graphW - 50) ? 'right' : 'left';\n");
        fprintf(f_html, "                        const labelX = (qx > padLeft + graphW - 50) ? qx - 5 : qx + 5;\n");
        fprintf(f_html, "                        bufCtx.fillText(`${qVal >= 0 ? '+' : ''}${qVal.toFixed(2)}`, labelX, Math.max(padTop + 12, Math.min(padTop + graphH - 4, scoreY)));\n");
        fprintf(f_html, "                    }\n");
        fprintf(f_html, "                });\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        bufCtx.restore();\n");

        fprintf(f_html, "        bufCtx.save(); bufCtx.translate(20, padTop + graphH / 2); bufCtx.rotate(-Math.PI / 2);\n");
        fprintf(f_html, "        bufCtx.fillStyle = '#7f8c8d'; bufCtx.font = '11px Segoe UI, sans-serif'; bufCtx.textAlign = 'center';\n");
        fprintf(f_html, "        bufCtx.fillText('Transience Flux / Energy', 0, 0); bufCtx.restore();\n");
        fprintf(f_html, "        bufCtx.fillStyle = '#7f8c8d'; bufCtx.font = '11px Segoe UI, sans-serif'; bufCtx.textAlign = 'center';\n");
        fprintf(f_html, "        bufCtx.fillText('Historical Window Time (ms)', padLeft + graphW / 2, padTop + graphH + 34);\n");

        fprintf(f_html, "        bufCtx.fillStyle = '#2c3e50'; bufCtx.font = 'bold 13px Segoe UI, sans-serif'; bufCtx.textAlign = 'left';\n");
        fprintf(f_html, "        if (Math.abs(zoomStartMs - (-pass2WinMs)) > 1e-2 || Math.abs(zoomEndMs - 0.0) > 1e-2) {\n");
        fprintf(f_html, "            bufCtx.fillText(`Accumulated Historical Buffer [Zoomed: ${Math.round(zoomStartMs)}ms to ${Math.round(zoomEndMs)}ms]`, padLeft, padTop - 10);\n");
        fprintf(f_html, "        } else {\n");
        fprintf(f_html, "            bufCtx.fillText(`Accumulated ${Math.round(pass2WinMs)}ms Historical Buffer`, padLeft, padTop - 10);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        const latestActivePeak = getLatestActivePeak();\n");
        fprintf(f_html, "        if (latestActivePeak) {\n");
        fprintf(f_html, "            bufCtx.fillStyle = getScoreColor(latestActivePeak.total_score, 1.0);\n");
        fprintf(f_html, "            bufCtx.font = 'bold 13px Segoe UI, sans-serif'; bufCtx.textAlign = 'right';\n");
        fprintf(f_html, "            bufCtx.fillText(latestActivePeak.total_score.toFixed(3), padLeft + graphW, padTop - 10);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        if (isDraggingBuf) {\n");
        fprintf(f_html, "            const selX = Math.min(dragStartX, dragCurrentX), selW = Math.abs(dragCurrentX - dragStartX);\n");
        fprintf(f_html, "            bufCtx.fillStyle = 'rgba(52, 152, 219, 0.25)'; bufCtx.fillRect(selX, padTop, selW, graphH);\n");
        fprintf(f_html, "            bufCtx.strokeStyle = '#2980b9'; bufCtx.lineWidth = 1.5; bufCtx.setLineDash([3, 3]);\n");
        fprintf(f_html, "            bufCtx.strokeRect(selX, padTop, selW, graphH); bufCtx.setLineDash([]);\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "    }\n\n");


        fprintf(f_html, "    if (bufCanvas && bufCtx) {\n");
        fprintf(f_html, "        function getCanvasMouseX(e) {\n");
        fprintf(f_html, "            const rect = bufCanvas.getBoundingClientRect();\n");
        fprintf(f_html, "            if (!rect.width) return 0;\n");
        fprintf(f_html, "            const scaleX = bufCanvas.width / rect.width;\n");
        fprintf(f_html, "            return (e.clientX - rect.left) * scaleX;\n");
        fprintf(f_html, "        }\n");
        fprintf(f_html, "        bufCanvas.addEventListener('contextmenu', (e) => {\n");
        fprintf(f_html, "            e.preventDefault(); zoomStartMs = -pass2WinMs; zoomEndMs = 0.0;\n");
        fprintf(f_html, "            isDraggingBuf = false; drawHistoryBuffer();\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "        bufCanvas.addEventListener('mousedown', (e) => {\n");
        fprintf(f_html, "            if (e.button !== 0) return;\n");
        fprintf(f_html, "            const clickX = getCanvasMouseX(e);\n");
        fprintf(f_html, "            const padLeft = 70, graphW = bufCanvas.width - 70 - 35;\n");
        fprintf(f_html, "            if (clickX >= padLeft && clickX <= padLeft + graphW) {\n");
        fprintf(f_html, "                isDraggingBuf = true; dragStartX = clickX; dragCurrentX = clickX;\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "        bufCanvas.addEventListener('mousemove', (e) => {\n");
        fprintf(f_html, "            if (!isDraggingBuf) return;\n");
        fprintf(f_html, "            const mouseX = getCanvasMouseX(e);\n");
        fprintf(f_html, "            const padLeft = 70, graphW = bufCanvas.width - 70 - 35;\n");
        fprintf(f_html, "            dragCurrentX = Math.max(padLeft, Math.min(padLeft + graphW, mouseX));\n");
        fprintf(f_html, "            drawHistoryBuffer();\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "        bufCanvas.addEventListener('mouseup', (e) => {\n");
        fprintf(f_html, "            if (!isDraggingBuf || e.button !== 0) return;\n");
        fprintf(f_html, "            isDraggingBuf = false;\n");
        fprintf(f_html, "            const padLeft = 70, graphW = bufCanvas.width - 70 - 35;\n");
        fprintf(f_html, "            const dx = Math.abs(dragCurrentX - dragStartX);\n");
        fprintf(f_html, "            if (dx > 5) {\n");
        fprintf(f_html, "                const x1 = Math.min(dragStartX, dragCurrentX), x2 = Math.max(dragStartX, dragCurrentX);\n");
        fprintf(f_html, "                const frac1 = (x1 - padLeft) / graphW, frac2 = (x2 - padLeft) / graphW;\n");
        fprintf(f_html, "                const currentSpan = zoomEndMs - zoomStartMs;\n");
        fprintf(f_html, "                const newStartMs = zoomStartMs + frac1 * currentSpan, newEndMs = zoomStartMs + frac2 * currentSpan;\n");
        fprintf(f_html, "                if (newEndMs - newStartMs >= 10.0) { zoomStartMs = newStartMs; zoomEndMs = newEndMs; }\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "            drawHistoryBuffer();\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "        bufCanvas.addEventListener('mouseleave', () => {\n");
        fprintf(f_html, "            if (isDraggingBuf) { isDraggingBuf = false; drawHistoryBuffer(); }\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function renderAll() {\n");
        fprintf(f_html, "        drawClusterHistogram();\n");
        fprintf(f_html, "        drawMidpointBuffer();\n");
        fprintf(f_html, "        drawWaveformMap();\n");
        fprintf(f_html, "        updateSegmentInspector();\n");
        fprintf(f_html, "        drawHistoryBuffer();\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    if (canvas) {\n");
        fprintf(f_html, "        canvas.addEventListener('click', (e) => {\n");
        fprintf(f_html, "            const rect = canvas.getBoundingClientRect();\n");
        fprintf(f_html, "            const clickX = e.clientX - rect.left;\n");
        fprintf(f_html, "            const clickFraction = Math.max(0, Math.min(1, clickX / rect.width));\n");
        fprintf(f_html, "            const dur = (audio.duration && !isNaN(audio.duration) && audio.duration > 0) ? audio.duration : totalDurationS;\n");
        fprintf(f_html, "            if (dur > 0) {\n");
        fprintf(f_html, "                const wasPaused = audio.paused;\n");
        fprintf(f_html, "                audio.currentTime = clickFraction * dur;\n");
        fprintf(f_html, "                if (!wasPaused) audio.play().catch(e => console.log(e));\n");
        fprintf(f_html, "                renderAll();\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    function setupSegmentClickListener(canvasId) {\n");
        fprintf(f_html, "        const canvasElem = document.getElementById(canvasId);\n");
        fprintf(f_html, "        if (!canvasElem) return;\n");
        fprintf(f_html, "        canvasElem.addEventListener('click', (e) => {\n");
        fprintf(f_html, "            if (!canvasElem.dataset.startMs || !canvasElem.dataset.endMs) return;\n");
        fprintf(f_html, "            const startMs = parseFloat(canvasElem.dataset.startMs);\n");
        fprintf(f_html, "            const endMs = parseFloat(canvasElem.dataset.endMs);\n");
        fprintf(f_html, "            const segDurMs = endMs - startMs;\n");
        fprintf(f_html, "            if (segDurMs <= 0) return;\n");
        fprintf(f_html, "            const rect = canvasElem.getBoundingClientRect();\n");
        fprintf(f_html, "            const clickX = e.clientX - rect.left;\n");
        fprintf(f_html, "            const scaleX = canvasElem.width / rect.width;\n");
        fprintf(f_html, "            const canvasX = clickX * scaleX;\n");
        fprintf(f_html, "            const clickFraction = Math.max(0, Math.min(1, canvasX / canvasElem.width));\n");
        fprintf(f_html, "            const targetTimeS = (startMs + clickFraction * segDurMs) / 1000.0;\n");
        fprintf(f_html, "            const dur = (audio.duration && !isNaN(audio.duration) && audio.duration > 0) ? audio.duration : totalDurationS;\n");
        fprintf(f_html, "            if (targetTimeS >= 0 && targetTimeS <= dur) {\n");
        fprintf(f_html, "                const wasPaused = audio.paused;\n");
        fprintf(f_html, "                audio.currentTime = targetTimeS;\n");
        fprintf(f_html, "                if (!wasPaused) audio.play().catch(e => console.log(e));\n");
        fprintf(f_html, "                renderAll();\n");
        fprintf(f_html, "            }\n");
        fprintf(f_html, "        });\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    setupSegmentClickListener('canvasPrev');\n");
        fprintf(f_html, "    setupSegmentClickListener('canvasCurr');\n");
        fprintf(f_html, "    setupSegmentClickListener('canvasNext');\n\n");

        fprintf(f_html, "    if (window.snapshotsBase64) {\n");
        fprintf(f_html, "        try {\n");
        fprintf(f_html, "            const binaryString = atob(window.snapshotsBase64);\n");
        fprintf(f_html, "            const bytes = new Uint8Array(binaryString.length);\n");
        fprintf(f_html, "            for (let i = 0; i < binaryString.length; i++) bytes[i] = binaryString.charCodeAt(i);\n");
        fprintf(f_html, "            window.snapshotsArrayBuffer = bytes.buffer;\n");
        fprintf(f_html, "        } catch (e) { console.log('Base64 decode error:', e); }\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    if (window.reportData) {\n");
        fprintf(f_html, "        applyReportData(window.reportData);\n");
        fprintf(f_html, "    } else {\n");
        fprintf(f_html, "        fetch('report_data.json')\n");
        fprintf(f_html, "            .then(res => res.json())\n");
        fprintf(f_html, "            .then(data => applyReportData(data))\n");
        fprintf(f_html, "            .catch(e => console.log('report_data.json fetch notice:', e));\n");
        fprintf(f_html, "    }\n\n");

        fprintf(f_html, "    if (audio) {\n");
        fprintf(f_html, "        audio.addEventListener('timeupdate', renderAll);\n");
        fprintf(f_html, "        audio.addEventListener('play', renderAll);\n");
        fprintf(f_html, "        audio.addEventListener('pause', renderAll);\n");
        fprintf(f_html, "    }\n");
        fprintf(f_html, "</script>\n");
        fprintf(f_html, "</body>\n");
        fprintf(f_html, "</html>\n");
        fclose(f_html);
    }

    analyzer_free_analysis(&res2);
    return 1;
}

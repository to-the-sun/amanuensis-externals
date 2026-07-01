#define _USE_MATH_DEFINES
#include "cumulative_transience.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N_FFT 2048
#define N_MELS 128
#define CACHE_SIZE 15201 // Enough for 15.2 seconds at 1ms hop

// Internal FFT implementation: Simple Radix-2 FFT
static void fft(double* real, double* imag, int n) {
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            double tr = real[i]; real[i] = real[j]; real[j] = tr;
            double ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
        }
        int k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }

    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len;
        double wlen_r = cos(ang);
        double wlen_i = -sin(ang);
        for (int i = 0; i < n; i += len) {
            double w_r = 1.0;
            double w_i = 0.0;
            for (int k = 0; k < len / 2; k++) {
                double u_r = real[i + k];
                double u_i = imag[i + k];
                double v_r = real[i + k + len / 2] * w_r - imag[i + k + len / 2] * w_i;
                double v_i = real[i + k + len / 2] * w_i + imag[i + k + len / 2] * w_r;
                real[i + k] = u_r + v_r;
                imag[i + k] = u_i + v_i;
                real[i + k + len / 2] = u_r - v_r;
                imag[i + k + len / 2] = u_i - v_i;
                double tmp_r = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = tmp_r;
            }
        }
    }
}

typedef struct {
    int p_idx;
    int band_idx;
} PeakRef;

static int compare_peaks(const void* a, const void* b) {
    PeakRef* pa = (PeakRef*)a;
    PeakRef* pb = (PeakRef*)b;
    return pa->p_idx - pb->p_idx;
}

static double* create_mel_filterbank(int sr, int n_fft, int n_mels);

TransientAnalyzer* analyzer_create(double max_peak_value) {
    TransientAnalyzer* self = (TransientAnalyzer*)calloc(1, sizeof(TransientAnalyzer));
    if (!self) return NULL;

    self->max_peak = max_peak_value;
    for (int i = 0; i < BUFFER_LEN; i++) {
        self->buffer_times[i] = -5000.0 + i;
    }

    self->min_score_seen = 0.0;
    self->max_score_seen = 0.0;
    self->last_score_avg = 0.0;
    self->frame_duration_ms = 1.0;

    // Cache pre-allocation
    self->mel_spectrogram = (double*)calloc(N_MELS * CACHE_SIZE, sizeof(double));
    self->flux_envelopes = (float*)calloc(MAX_BANDS * CACHE_SIZE, sizeof(float));
    self->fft_window = (double*)malloc(sizeof(double) * N_FFT);
    for (int i = 0; i < N_FFT; i++) {
        self->fft_window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (double)N_FFT));
    }
    self->sample_rate = 44100;
    self->mel_filters = create_mel_filterbank(self->sample_rate, N_FFT, N_MELS);
    self->cache_write_ptr = 0;
    self->cache_count = 0;
    self->max_mel_db = -DBL_MAX;
    self->overlap_buffer = (float*)calloc(N_FFT, sizeof(float));
    self->overlap_len = 0;

    return self;
}

void analyzer_destroy(TransientAnalyzer* self) {
    if (self->overlap_buffer) free(self->overlap_buffer);
    for (int b = 0; b < MAX_BANDS; b++) {
        SnapshotEntry* curr = self->snapshot_heads[b];
        while (curr) {
            SnapshotEntry* next = curr->next;
            free(curr);
            curr = next;
        }
    }
    if (self->mel_spectrogram) free(self->mel_spectrogram);
    if (self->flux_envelopes) free(self->flux_envelopes);
    if (self->fft_window) free(self->fft_window);
    if (self->mel_filters) free(self->mel_filters);
    free(self);
}

void analyzer_set_sample_rate(TransientAnalyzer* self, int sr) {
    if (self->sample_rate != sr) {
        self->sample_rate = sr;
        if (self->mel_filters) free(self->mel_filters);
        self->mel_filters = create_mel_filterbank(sr, N_FFT, N_MELS);
    }
    int hop_length = (int)(sr * 0.001);
    self->frame_duration_ms = 1000.0 * (double)hop_length / (double)sr;
    for (int i = 0; i < BUFFER_LEN; i++) {
        self->buffer_times[i] = (double)(i - 5000) * self->frame_duration_ms;
    }
}

double analyzer_get_max_peak(TransientAnalyzer* self) {
    return self->max_peak;
}

int analyzer_process_peak(TransientAnalyzer* self,
                          int p_idx,
                          int band_idx,
                          double time,
                          const float* env_ptr,
                          int env_len,
                          const int* all_valid_peak_indices,
                          int all_valid_count,
                          double detected_peak_val,
                          double thresh_val,
                          double left_min,
                          double right_min,
                          double prominence,
                          PeakResult* result_out) {

    result_out->p_idx = p_idx;
    result_out->band_idx = band_idx;
    result_out->time = time;
    result_out->peak_val = (double)env_ptr[p_idx];
    result_out->total_score = 0;
    result_out->detected_peak_val = detected_peak_val;
    result_out->thresh_val = thresh_val;
    result_out->left_min = left_min;
    result_out->right_min = right_min;
    result_out->prominence = prominence;
    result_out->num_qualifiers = 0;

    int start = p_idx - 5000;

    // Fill snapshot
    for (int i = 0; i < BUFFER_LEN; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= env_len) {
            result_out->snapshot[i] = 0.0;
        } else {
            result_out->snapshot[i] = (double)env_ptr[idx];
        }
    }

    double normalization = (self->max_peak > 0) ? (result_out->peak_val / self->max_peak) : 1.0;
    for (int i = 0; i < BUFFER_LEN; i++) {
        result_out->snapshot[i] *= normalization;
    }

    // Calculate Resonance Score
    double qualifier_sum = 0.0;
    bool found_peak = false;

    // data_to_measure is accumulated_buffer[:-99]
    int m_len = BUFFER_LEN - 99;
    double sum = 0.0;
    double max_v = -DBL_MAX;
    double min_v = DBL_MAX;

    if (m_len > 0) {
        max_v = self->accumulated_buffer[0];
        min_v = self->accumulated_buffer[0];
        for (int i = 0; i < m_len; i++) {
            double val = self->accumulated_buffer[i];
            sum += val;
            if (val > max_v) max_v = val;
            if (val < min_v) min_v = val;
        }
    }
    double avg = (m_len > 0) ? (sum / (double)m_len) : 0.0;

    for (int i = 0; i < all_valid_count; i++) {
        int s_idx = all_valid_peak_indices[i];
        if (s_idx >= p_idx - 5000 && s_idx <= p_idx - 99) {
            int sp_idx = 5000 - (p_idx - s_idx);
            double val = self->accumulated_buffer[sp_idx];
            double qualifier = 0.0;
            if (val > avg) {
                if (max_v > avg) qualifier = (val - avg) / (max_v - avg);
            } else if (val < avg) {
                if (avg > min_v) qualifier = (val - avg) / (avg - min_v);
            }

            if (result_out->num_qualifiers < MAX_QUALIFIERS) {
                result_out->qualifiers[result_out->num_qualifiers].ms = self->buffer_times[sp_idx];
                result_out->qualifiers[result_out->num_qualifiers].val = qualifier;
                result_out->num_qualifiers++;
            }
            qualifier_sum += qualifier;
            found_peak = true;
        }
    }

    if (found_peak) {
        result_out->total_score = result_out->peak_val * qualifier_sum;
    }

    // Update dynamic range
    if (result_out->total_score < self->min_score_seen) self->min_score_seen = result_out->total_score;
    if (result_out->total_score > self->max_score_seen) self->max_score_seen = result_out->total_score;

    self->total_score_sum += result_out->total_score;
    self->score_count++;


    // Update accumulated buffer
    for (int i = 0; i < BUFFER_LEN; i++) {
        self->accumulated_buffer[i] += result_out->snapshot[i];
    }

    // Store snapshot for later cleanup (queue based)
    SnapshotEntry* entry = (SnapshotEntry*)malloc(sizeof(SnapshotEntry));
    entry->p_idx = p_idx;
    memcpy(entry->snapshot, result_out->snapshot, sizeof(double) * BUFFER_LEN);
    entry->next = NULL;

    if (self->snapshot_tails[band_idx]) {
        self->snapshot_tails[band_idx]->next = entry;
        self->snapshot_tails[band_idx] = entry;
    } else {
        self->snapshot_heads[band_idx] = entry;
        self->snapshot_tails[band_idx] = entry;
    }

    return 1;
}

void analyzer_update_metrics(TransientAnalyzer* self, int frame, AnalyzerMetrics* metrics_out) {
    int cleanup_frame_threshold = frame - 15000;
    bool buffer_updated = false;

    for (int b = 0; b < MAX_BANDS; b++) {
        while (self->snapshot_heads[b] && self->snapshot_heads[b]->p_idx <= cleanup_frame_threshold) {
            SnapshotEntry* entry = self->snapshot_heads[b];
            for (int j = 0; j < BUFFER_LEN; j++) {
                self->accumulated_buffer[j] -= entry->snapshot[j];
            }
            self->snapshot_heads[b] = entry->next;
            if (!self->snapshot_heads[b]) self->snapshot_tails[b] = NULL;
            free(entry);
            buffer_updated = true;
        }
    }

    int m_len = BUFFER_LEN - 99;
    double sum = 0.0, sum_sq = 0.0;
    double max_v = -DBL_MAX;

    if (m_len > 0) {
        max_v = self->accumulated_buffer[0];
        for (int i = 0; i < m_len; i++) {
            double val = self->accumulated_buffer[i];
            sum += val;
            sum_sq += val * val;
            if (val > max_v) max_v = val;
        }
    }

    double mean = (m_len > 0) ? (sum / (double)m_len) : 0.0;
    double variance = (m_len > 0) ? (sum_sq / (double)m_len - mean * mean) : 0.0;
    if (variance < 0) variance = 0;
    double std_dev = sqrt(variance);

    metrics_out->std_dev = std_dev;
    metrics_out->mean = mean;
    metrics_out->contrast = (mean > 0) ? (max_v / mean) : 0;
    metrics_out->rating = (self->score_count > 0) ? (self->total_score_sum / self->score_count) : 0;
    metrics_out->buffer_updated = buffer_updated;
    metrics_out->min_score_seen = self->min_score_seen;
    metrics_out->max_score_seen = self->max_score_seen;
    metrics_out->highest_peak_valid = false;

    if (max_v > 0.1) {
        int highest_idx = -1;
        double highest_val = -DBL_MAX;
        for (int i = 0; i < m_len; i++) {
            double val = self->accumulated_buffer[i];
            if (val > mean && val > highest_val) {
                highest_val = val;
                highest_idx = i;
            }
        }

        if (highest_idx != -1) {
            metrics_out->highest_peak_ms = self->buffer_times[highest_idx];
            metrics_out->highest_peak_valid = true;

            if (self->peak_history_count < MAX_PEAK_HISTORY) {
                self->peak_history[self->peak_history_count++] = metrics_out->highest_peak_ms;
            }
        }
    }

    double ph_sum = 0, ph_sum_sq = 0;
    for (int i = 0; i < self->peak_history_count; i++) {
        ph_sum += self->peak_history[i];
        ph_sum_sq += self->peak_history[i] * self->peak_history[i];
    }
    double ph_mean = (self->peak_history_count > 0) ? (ph_sum / self->peak_history_count) : 0;
    double ph_var = (self->peak_history_count > 0) ? (ph_sum_sq / self->peak_history_count - ph_mean * ph_mean) : 0;
    if (ph_var < 0) ph_var = 0;
    metrics_out->peak_std = ph_var > 0 ? sqrt(ph_var) : 0;

}

double* analyzer_get_buffer(TransientAnalyzer* self) {
    return self->accumulated_buffer;
}

void analyzer_push_audio(TransientAnalyzer* self, const float* y, int len, int sr) {
    if (self->sample_rate != sr) {
        analyzer_set_sample_rate(self, sr);
        self->overlap_len = 0; // Reset overlap on SR change
    }

    int hop_length = (int)(sr * 0.001);
    int combined_len = self->overlap_len + len;
    float* combined = (float*)malloc(sizeof(float) * combined_len);
    if (self->overlap_len > 0) {
        memcpy(combined, self->overlap_buffer, sizeof(float) * self->overlap_len);
    }
    memcpy(combined + self->overlap_len, y, sizeof(float) * len);

    int num_frames = len / hop_length;
    double* real = (double*)malloc(sizeof(double) * N_FFT);
    double* imag = (double*)malloc(sizeof(double) * N_FFT);

    for (int f = 0; f < num_frames; f++) {
        int center = f * hop_length + self->overlap_len;
        int start = center - N_FFT / 2;

        for (int i = 0; i < N_FFT; i++) {
            int idx = start + i;
            int r_idx = idx;
            if (r_idx < 0) r_idx = -r_idx;
            if (r_idx >= combined_len) r_idx = 2 * combined_len - 2 - r_idx;
            real[i] = (r_idx >= 0 && r_idx < combined_len) ? (double)combined[r_idx] * self->fft_window[i] : 0.0;
            imag[i] = 0.0;
        }

        fft(real, imag, N_FFT);

        int f_idx = self->cache_write_ptr;
        double frame_max_db = -DBL_MAX;
        for (int m = 0; m < N_MELS; m++) {
            double mel_val = 0;
            for (int i = 0; i < (N_FFT / 2 + 1); i++) {
                // Normalize power by N^2 (Forward gain is N)
                double power = (real[i] * real[i] + imag[i] * imag[i]) / ((double)N_FFT * (double)N_FFT);
                mel_val += power * self->mel_filters[m * (N_FFT / 2 + 1) + i];
            }
            if (mel_val < 1e-10) mel_val = 1e-10;
            double db_val = 10.0 * log10(mel_val);
            self->mel_spectrogram[m * CACHE_SIZE + f_idx] = db_val;
            if (db_val > frame_max_db) frame_max_db = db_val;
        }

        if (frame_max_db > self->max_mel_db) self->max_mel_db = frame_max_db;

        double floor_db = self->max_mel_db - 80.0;
        for (int m = 0; m < N_MELS; m++) {
            if (self->mel_spectrogram[m * CACHE_SIZE + f_idx] < floor_db) {
                self->mel_spectrogram[m * CACHE_SIZE + f_idx] = floor_db;
            }
        }

        int prev_f_idx = (f_idx - 1 + CACHE_SIZE) % CACHE_SIZE;
        for (int b = 0; b < MAX_BANDS; b++) {
            double flux_sum = 0;
            for (int m = b * 32; m < (b + 1) * 32; m++) {
                double diff = self->mel_spectrogram[m * CACHE_SIZE + f_idx] - self->mel_spectrogram[m * CACHE_SIZE + prev_f_idx];
                if (diff > 0) flux_sum += diff;
            }
            self->flux_envelopes[b * CACHE_SIZE + f_idx] = (float)(flux_sum / 32.0);
        }

        self->cache_write_ptr = (self->cache_write_ptr + 1) % CACHE_SIZE;
        if (self->cache_count < CACHE_SIZE) self->cache_count++;
    }

    // Save tail for next overlap (preserves lookback context)
    int used_up_to = num_frames * hop_length;
    int remaining = combined_len - used_up_to;
    if (remaining > N_FFT) remaining = N_FFT;
    if (remaining > 0) {
        memcpy(self->overlap_buffer, combined + (combined_len - remaining), sizeof(float) * remaining);
        self->overlap_len = remaining;
    } else {
        self->overlap_len = 0;
    }

    free(combined);
    free(real);
    free(imag);
}

int analyzer_analyze_chunk(TransientAnalyzer* self,
                           const float* y,
                           int len,
                           int sr,
                           int buffer_start_frame,
                           int active_start_frame,
                           ChunkAnalysisResult* result_out) {

    analyzer_push_audio(self, y, len, sr);

    int num_frames = self->cache_count;
    int read_ptr = (self->cache_write_ptr - num_frames + CACHE_SIZE) % CACHE_SIZE;

    BandAnalysis bands[MAX_BANDS];
    for (int b = 0; b < MAX_BANDS; b++) {
        bands[b].envelope = (float*)malloc(sizeof(float) * num_frames);
        bands[b].rolling_threshold = (float*)malloc(sizeof(float) * num_frames);

        double current_sum = 0;
        int window_size = 15000;
        for (int j = 0; j < num_frames; j++) {
            int f_idx = (read_ptr + j) % CACHE_SIZE;
            float flux_val = self->flux_envelopes[b * CACHE_SIZE + f_idx];
            bands[b].envelope[j] = flux_val;

            current_sum += (double)flux_val;
            if (j >= window_size) {
                current_sum -= (double)bands[b].envelope[j - window_size];
                bands[b].rolling_threshold[j] = (float)(current_sum / (double)window_size);
            } else {
                bands[b].rolling_threshold[j] = (float)(current_sum / (double)(j + 1));
            }
        }
    }

    for (int b = 0; b < MAX_BANDS; b++) {
        float* env = bands[b].envelope;
        float* thresh = bands[b].rolling_threshold;
        int* temp_peaks = (int*)malloc(sizeof(int) * num_frames);
        float* temp_thresh = (float*)malloc(sizeof(float) * num_frames);
        float* temp_left = (float*)malloc(sizeof(float) * num_frames);
        float* temp_right = (float*)malloc(sizeof(float) * num_frames);
        float* temp_prom = (float*)malloc(sizeof(float) * num_frames);
        int peak_count = 0;

        for (int f = 1; f < num_frames - 1; f++) {
            if (env[f] > env[f-1] && env[f] > env[f+1] && env[f] > thresh[f] && env[f] >= 3.0f) {
                bool replaced = false;
                bool too_close = false;
                if (peak_count > 0 && f - temp_peaks[peak_count-1] < 200) {
                    too_close = true;
                    if (env[f] > env[temp_peaks[peak_count-1]]) replaced = true;
                }

                if (!too_close || replaced) {
                    float left_min = env[f];
                    for(int k=f-1; k>=0; k--) {
                        if (env[k] > env[f]) break;
                        if (env[k] < left_min) left_min = env[k];
                    }
                    float right_min = env[f];
                    for(int k=f+1; k<num_frames; k++) {
                        if (env[k] > env[f]) break;
                        if (env[k] < right_min) right_min = env[k];
                    }
                    float prom = env[f] - (left_min > right_min ? left_min : right_min);

                    if (prom >= 0.5f) {
                        if (replaced) {
                            temp_peaks[peak_count-1] = f;
                            temp_thresh[peak_count-1] = thresh[f];
                            temp_left[peak_count-1] = left_min;
                            temp_right[peak_count-1] = right_min;
                            temp_prom[peak_count-1] = prom;
                        } else if (!too_close) {
                            temp_peaks[peak_count] = f;
                            temp_thresh[peak_count] = thresh[f];
                            temp_left[peak_count] = left_min;
                            temp_right[peak_count] = right_min;
                            temp_prom[peak_count] = prom;
                            peak_count++;
                        }
                    }
                }
            }
        }

        bands[b].peaks = (int*)malloc(sizeof(int) * peak_count);
        bands[b].thresh_vals = (float*)malloc(sizeof(float) * peak_count);
        bands[b].left_mins = (float*)malloc(sizeof(float) * peak_count);
        bands[b].right_mins = (float*)malloc(sizeof(float) * peak_count);
        bands[b].proms = (float*)malloc(sizeof(float) * peak_count);

        memcpy(bands[b].peaks, temp_peaks, sizeof(int) * peak_count);
        memcpy(bands[b].thresh_vals, temp_thresh, sizeof(float) * peak_count);
        memcpy(bands[b].left_mins, temp_left, sizeof(float) * peak_count);
        memcpy(bands[b].right_mins, temp_right, sizeof(float) * peak_count);
        memcpy(bands[b].proms, temp_prom, sizeof(float) * peak_count);

        bands[b].num_peaks = peak_count;
        free(temp_peaks); free(temp_thresh); free(temp_left); free(temp_right); free(temp_prom);
    }

    float global_max = 0;
    bool any_peak = false;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < bands[b].num_peaks; i++) {
            float val = bands[b].envelope[bands[b].peaks[i]];
            if (!any_peak || val > global_max) { global_max = val; any_peak = true; }
        }
    }
    if (any_peak && global_max > (float)self->max_peak) self->max_peak = (double)global_max;

    int total_peaks = 0;
    for (int b = 0; b < MAX_BANDS; b++) total_peaks += bands[b].num_peaks;

    PeakRef* all_peaks_ref = (PeakRef*)malloc(sizeof(PeakRef) * (total_peaks + 1));
    int* all_indices = (int*)malloc(sizeof(int) * (total_peaks + 1));
    int curr = 0;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < bands[b].num_peaks; i++) {
            all_peaks_ref[curr].p_idx = bands[b].peaks[i];
            all_peaks_ref[curr].band_idx = b;
            all_indices[curr] = bands[b].peaks[i];
            curr++;
        }
    }
    qsort(all_peaks_ref, total_peaks, sizeof(PeakRef), compare_peaks);

    result_out->peak_list.num_peaks = 0;
    for (int i = 0; i < total_peaks; i++) {
        int p_idx = all_peaks_ref[i].p_idx;
        int b = all_peaks_ref[i].band_idx;
        int p_global = buffer_start_frame + p_idx;

        if (p_global >= active_start_frame && p_global < active_start_frame + 100) {
            double p_val = 0, t_val = 0, l_val = 0, r_val = 0, pr_val = 0;
            for (int k = 0; k < bands[b].num_peaks; k++) {
                if (bands[b].peaks[k] == p_idx) {
                    p_val = bands[b].envelope[p_idx];
                    t_val = bands[b].thresh_vals[k];
                    l_val = bands[b].left_mins[k];
                    r_val = bands[b].right_mins[k];
                    pr_val = bands[b].proms[k];
                    break;
                }
            }

            PeakResult pr;
            double time = (double)p_global * self->frame_duration_ms / 1000.0;
            if (analyzer_process_peak(self, p_idx, b, time, bands[b].envelope, num_frames, all_indices, total_peaks, p_val, t_val, l_val, r_val, pr_val, &pr)) {
                if (self->snapshot_tails[b]) self->snapshot_tails[b]->p_idx = p_global;
                if (result_out->peak_list.num_peaks < MAX_PEAKS_PER_CHUNK) {
                    pr.p_idx = p_global;
                    result_out->peak_list.peaks[result_out->peak_list.num_peaks++] = pr;
                }
            }
        }
    }

    analyzer_update_metrics(self, active_start_frame + 100, &result_out->metrics);

    for (int b = 0; b < MAX_BANDS; b++) {
        free(bands[b].envelope); free(bands[b].rolling_threshold);
        free(bands[b].peaks); free(bands[b].thresh_vals);
        free(bands[b].left_mins); free(bands[b].right_mins); free(bands[b].proms);
    }
    free(all_peaks_ref); free(all_indices);
    return 1;
}

static double* create_mel_filterbank(int sr, int n_fft, int n_mels) {
    double* filters = (double*)malloc(sizeof(double) * n_mels * (n_fft / 2 + 1));
    memset(filters, 0, sizeof(double) * n_mels * (n_fft / 2 + 1));
    double f_min = 0, f_max = sr / 2.0;
    static const double f_sp = 200.0 / 3.0, min_log_hz = 1000.0;
    const double min_log_mel = (min_log_hz - 0.0) / f_sp, log_step = log(6.4) / 27.0;

    #define HZ_TO_MEL(hz) ((hz) < min_log_hz ? ((hz) - 0.0) / f_sp : min_log_mel + log((hz) / min_log_hz) / log_step)
    #define MEL_TO_HZ(mel) ((mel) < min_log_mel ? 0.0 + (mel) * f_sp : min_log_hz * exp(log_step * ((mel) - min_log_mel)))

    double mel_min = HZ_TO_MEL(f_min), mel_max = HZ_TO_MEL(f_max);
    double* mel_points = (double*)malloc(sizeof(double) * (n_mels + 2));
    for (int i = 0; i < n_mels + 2; i++) {
        double mel = mel_min + i * (mel_max - mel_min) / (n_mels + 1);
        mel_points[i] = MEL_TO_HZ(mel);
    }

    int n_fft_bins = n_fft / 2 + 1;
    for (int j = 0; j < n_mels; j++) {
        double f_low = mel_points[j], f_mid = mel_points[j+1], f_high = mel_points[j+2];
        for (int i = 0; i < n_fft_bins; i++) {
            double f = i * (double)sr / n_fft;
            double weight = 0;
            if (f >= f_low && f <= f_mid) weight = (f - f_low) / (f_mid - f_low);
            else if (f >= f_mid && f <= f_high) weight = (f_high - f) / (f_high - f_mid);
            filters[j * n_fft_bins + i] = weight;
        }
        double enorm = 2.0 / (f_high - f_low);
        for (int i = 0; i < n_fft_bins; i++) filters[j * n_fft_bins + i] *= enorm;
    }
    free(mel_points);
    return filters;
}

int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out) {
    result_out->ratings = NULL; result_out->std_devs = NULL; result_out->contrasts = NULL; result_out->peak_stds = NULL;
    int hop_length = (int)(sr * 0.001);
    int num_frames = (len + hop_length - 1) / hop_length;
    result_out->num_frames = num_frames;
    result_out->times = (float*)malloc(sizeof(float) * num_frames);
    for (int i = 0; i < num_frames; i++) result_out->times[i] = (float)i * (float)hop_length / (float)sr;

    double* window = (double*)malloc(sizeof(double) * N_FFT);
    for (int i = 0; i < N_FFT; i++) window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (double)N_FFT));
    double* mel_filters = create_mel_filterbank(sr, N_FFT, N_MELS);
    double* mel_spectrogram = (double*)malloc(sizeof(double) * N_MELS * num_frames);
    double* real = (double*)malloc(sizeof(double) * N_FFT);
    double* imag = (double*)malloc(sizeof(double) * N_FFT);
    double rolling_max_mel_db = -DBL_MAX;

    for (int f = 0; f < num_frames; f++) {
        int center = f * hop_length;
        int start_idx = center - N_FFT / 2;
        for (int i = 0; i < N_FFT; i++) {
            int idx = start_idx + i;
            int r_idx = idx;
            if (r_idx < 0) r_idx = -r_idx;
            if (r_idx >= len) r_idx = 2 * len - 2 - r_idx;
            real[i] = (r_idx >= 0 && r_idx < len) ? (double)y[r_idx] * window[i] : 0.0;
            imag[i] = 0.0;
        }
        fft(real, imag, N_FFT);
        double frame_max_db = -DBL_MAX;
        for (int m = 0; m < N_MELS; m++) {
            double mel_val = 0;
            for (int i = 0; i < (N_FFT / 2 + 1); i++) {
                double power = (real[i] * real[i] + imag[i] * imag[i]) / ((double)N_FFT * (double)N_FFT);
                mel_val += power * mel_filters[m * (N_FFT / 2 + 1) + i];
            }
            if (mel_val < 1e-10) mel_val = 1e-10;
            double db_val = 10.0 * log10(mel_val);
            mel_spectrogram[m * num_frames + f] = db_val;
            if (db_val > frame_max_db) frame_max_db = db_val;
        }
        if (frame_max_db > rolling_max_mel_db) rolling_max_mel_db = frame_max_db;
        double floor_db = rolling_max_mel_db - 80.0;
        for (int m = 0; m < N_MELS; m++) {
            if (mel_spectrogram[m * num_frames + f] < floor_db) mel_spectrogram[m * num_frames + f] = floor_db;
        }
    }
    free(real); free(imag); free(window); free(mel_filters);

    for (int b = 0; b < MAX_BANDS; b++) {
        result_out->bands[b].envelope = (float*)malloc(sizeof(float) * num_frames);
        result_out->bands[b].envelope[0] = 0.0f;
        for (int f = 1; f < num_frames; f++) {
            double flux = 0;
            for (int m = b * 32; m < (b + 1) * 32; m++) {
                double diff = mel_spectrogram[m * num_frames + f] - mel_spectrogram[m * num_frames + f - 1];
                if (diff > 0) flux += diff;
            }
            result_out->bands[b].envelope[f] = (float)(flux / 32.0);
        }
        result_out->bands[b].rolling_threshold = (float*)malloc(sizeof(float) * num_frames);
        double current_sum = 0;
        int window_size = 15000;
        for (int f = 0; f < num_frames; f++) {
            current_sum += (double)result_out->bands[b].envelope[f];
            if (f >= window_size) {
                current_sum -= (double)result_out->bands[b].envelope[f - window_size];
                result_out->bands[b].rolling_threshold[f] = (float)(current_sum / (double)window_size);
            } else result_out->bands[b].rolling_threshold[f] = (float)(current_sum / (double)(f + 1));
        }
    }

    for (int b = 0; b < MAX_BANDS; b++) {
        float* env = result_out->bands[b].envelope;
        float* thresh = result_out->bands[b].rolling_threshold;
        int* temp_peaks = (int*)malloc(sizeof(int) * num_frames);
        int peak_count = 0;
        float* temp_thresh = (float*)malloc(sizeof(float) * num_frames);
        float* temp_left = (float*)malloc(sizeof(float) * num_frames);
        float* temp_right = (float*)malloc(sizeof(float) * num_frames);
        float* temp_prom = (float*)malloc(sizeof(float) * num_frames);

        for (int f = 1; f < num_frames - 1; f++) {
            if (env[f] > env[f-1] && env[f] > env[f+1] && env[f] > thresh[f] && env[f] >= 3.0f) {
                bool replaced = false, too_close = false;
                if (peak_count > 0 && f - temp_peaks[peak_count-1] < 200) {
                    too_close = true;
                    if (env[f] > env[temp_peaks[peak_count-1]]) replaced = true;
                }
                if (!too_close || replaced) {
                    float left_min = env[f];
                    for(int k=f-1; k>=0; k--) { if (env[k] > env[f]) break; if (env[k] < left_min) left_min = env[k]; }
                    float right_min = env[f];
                    for(int k=f+1; k<num_frames; k++) { if (env[k] > env[f]) break; if (env[k] < right_min) right_min = env[k]; }
                    float prom = env[f] - (left_min > right_min ? left_min : right_min);
                    if (prom >= 0.5f) {
                        if (replaced) {
                            temp_peaks[peak_count-1] = f; temp_thresh[peak_count-1] = thresh[f];
                            temp_left[peak_count-1] = left_min; temp_right[peak_count-1] = right_min; temp_prom[peak_count-1] = prom;
                        } else if (!too_close) {
                            temp_peaks[peak_count] = f; temp_thresh[peak_count] = thresh[f];
                            temp_left[peak_count] = left_min; temp_right[peak_count] = right_min; temp_prom[peak_count] = prom;
                            peak_count++;
                        }
                    }
                }
            }
        }
        result_out->bands[b].peaks = (int*)malloc(sizeof(int) * peak_count);
        result_out->bands[b].thresh_vals = (float*)malloc(sizeof(float) * peak_count);
        result_out->bands[b].left_mins = (float*)malloc(sizeof(float) * peak_count);
        result_out->bands[b].right_mins = (float*)malloc(sizeof(float) * peak_count);
        result_out->bands[b].proms = (float*)malloc(sizeof(float) * peak_count);
        memcpy(result_out->bands[b].peaks, temp_peaks, sizeof(int) * peak_count);
        memcpy(result_out->bands[b].thresh_vals, temp_thresh, sizeof(float) * peak_count);
        memcpy(result_out->bands[b].left_mins, temp_left, sizeof(float) * peak_count);
        memcpy(result_out->bands[b].right_mins, temp_right, sizeof(float) * peak_count);
        memcpy(result_out->bands[b].proms, temp_prom, sizeof(float) * peak_count);
        result_out->bands[b].num_peaks = peak_count;
        free(temp_peaks); free(temp_thresh); free(temp_left); free(temp_right); free(temp_prom);
    }
    float global_max = 0; bool any_peak = false;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
            float val = result_out->bands[b].envelope[result_out->bands[b].peaks[i]];
            if (!any_peak || val > global_max) { global_max = val; any_peak = true; }
        }
    }
    result_out->max_peak_value = any_peak ? global_max : 1.0f;
    free(mel_spectrogram);
    return 1;
}

int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out) {
    if (!analyzer_analyze_audio(y, len, sr, result_out)) return 0;
    int num_frames = result_out->num_frames;
    result_out->ratings = (double*)malloc(sizeof(double) * num_frames);
    result_out->std_devs = (double*)malloc(sizeof(double) * num_frames);
    result_out->contrasts = (double*)malloc(sizeof(double) * num_frames);
    result_out->peak_stds = (double*)malloc(sizeof(double) * num_frames);
    TransientAnalyzer* analyzer = analyzer_create(result_out->max_peak_value);
    analyzer_set_sample_rate(analyzer, sr);
    int total_peaks = 0;
    for (int b = 0; b < MAX_BANDS; b++) total_peaks += result_out->bands[b].num_peaks;
    int* all_valid_peaks = (int*)malloc(sizeof(int) * total_peaks);
    int curr = 0;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < result_out->bands[b].num_peaks; i++) all_valid_peaks[curr++] = result_out->bands[b].peaks[i];
    }
    for (int f = 0; f < num_frames; f++) {
        for (int b = 0; b < MAX_BANDS; b++) {
            for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
                if (result_out->bands[b].peaks[i] == f) {
                    PeakResult pr;
                    analyzer_process_peak(analyzer, f, b, result_out->times[f], result_out->bands[b].envelope, num_frames, all_valid_peaks, total_peaks,
                                          result_out->bands[b].envelope[f], result_out->bands[b].thresh_vals[i],
                                          result_out->bands[b].left_mins[i], result_out->bands[b].right_mins[i], result_out->bands[b].proms[i], &pr);
                }
            }
        }
        AnalyzerMetrics m;
        analyzer_update_metrics(analyzer, f, &m);
        result_out->ratings[f] = m.rating; result_out->std_devs[f] = m.std_dev; result_out->contrasts[f] = m.contrast; result_out->peak_stds[f] = m.peak_std;
    }
    free(all_valid_peaks); analyzer_destroy(analyzer);
    return 1;
}

void analyzer_free_analysis(FullAnalysisResult* result) {
    if (result->times) free(result->times);
    for (int i = 0; i < MAX_BANDS; i++) {
        if (result->bands[i].envelope) free(result->bands[i].envelope);
        if (result->bands[i].rolling_threshold) free(result->bands[i].rolling_threshold);
        if (result->bands[i].peaks) free(result->bands[i].peaks);
        if (result->bands[i].thresh_vals) free(result->bands[i].thresh_vals);
        if (result->bands[i].left_mins) free(result->bands[i].left_mins);
        if (result->bands[i].right_mins) free(result->bands[i].right_mins);
        if (result->bands[i].proms) free(result->bands[i].proms);
    }
    if (result->ratings) free(result->ratings);
    if (result->std_devs) free(result->std_devs);
    if (result->contrasts) free(result->contrasts);
    if (result->peak_stds) free(result->peak_stds);
}

void analyzer_debug_mel_filters(int sr, int n_fft, int n_mels, double* filters_out) {
    double* filters = create_mel_filterbank(sr, n_fft, n_mels);
    memcpy(filters_out, filters, sizeof(double) * n_mels * (n_fft / 2 + 1));
    free(filters);
}

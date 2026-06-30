#define _USE_MATH_DEFINES
#include "cumulative_transience.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

static int compare_floats(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

#define N_FFT 2048
#define N_MELS 128
#define CACHE_SIZE 15201 // Enough for 15.2 seconds at 1ms hop

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
    self->energy_envelopes = (float*)calloc(MAX_BANDS * CACHE_SIZE, sizeof(float));
    self->fft_window = (double*)malloc(sizeof(double) * N_FFT);
    for (int i = 0; i < N_FFT; i++) {
        self->fft_window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (double)N_FFT));
    }
    self->sample_rate = 44100;
    self->mel_filters = create_mel_filterbank(self->sample_rate, N_FFT, N_MELS);
    self->cache_write_ptr = 0;
    self->cache_count = 0;

    return self;
}

void analyzer_destroy(TransientAnalyzer* self) {
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
    if (self->energy_envelopes) free(self->energy_envelopes);
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

int analyzer_process_peak(TransientAnalyzer* self,
                          int p_idx,
                          int band_idx,
                          double time,
                          const float* env_ptr,
                          int env_len,
                          const int* all_valid_peak_indices,
                          int all_valid_count,
                          PeakResult* result_out) {

    result_out->p_idx = p_idx;
    result_out->band_idx = band_idx;
    result_out->time = time;
    result_out->peak_val = (double)env_ptr[p_idx];
    result_out->total_score = 0;
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
    }

    int hop_length = (int)(sr * 0.001);
    int num_frames = (len + hop_length - 1) / hop_length;

    double* real = (double*)malloc(sizeof(double) * N_FFT);
    double* imag = (double*)malloc(sizeof(double) * N_FFT);

    for (int f = 0; f < num_frames; f++) {
        int start = f * hop_length - N_FFT / 2;

        for (int i = 0; i < N_FFT; i++) {
            int idx = start + i;
            if (idx < 0) {
                idx = -idx;
            } else if (idx >= len) {
                idx = 2 * len - 2 - idx;
            }

            if (idx >= 0 && idx < len) {
                real[i] = (double)y[idx] * self->fft_window[i];
            } else {
                real[i] = 0;
            }
            imag[i] = 0;
        }

        fft(real, imag, N_FFT);

        // Current write position in circular buffer
        int f_idx = self->cache_write_ptr;

        for (int m = 0; m < N_MELS; m++) {
            double mel_val = 0;
            for (int i = 0; i < (N_FFT / 2 + 1); i++) {
                double re = real[i];
                double im = imag[i];
                mel_val += (re * re + im * im) * self->mel_filters[m * (N_FFT / 2 + 1) + i];
            }

            if (mel_val < 1e-10) mel_val = 1e-10;
            double db_val = 10.0 * log10(mel_val);
            self->mel_spectrogram[m * CACHE_SIZE + f_idx] = db_val;
        }

        // Calculate Flux for this frame across bands
        int prev_f_idx = (f_idx - 1 + CACHE_SIZE) % CACHE_SIZE;

        for (int b = 0; b < MAX_BANDS; b++) {
            double flux = 0;
            double energy = 0;
            for (int m = b * 32; m < (b + 1) * 32; m++) {
                double cur_db = self->mel_spectrogram[m * CACHE_SIZE + f_idx];
                double prev_db = self->mel_spectrogram[m * CACHE_SIZE + prev_f_idx];
                double diff = cur_db - prev_db;
                if (diff > 0) flux += diff;

                // Approximate linear energy from dB
                energy += pow(10.0, cur_db / 10.0);
            }
            self->flux_envelopes[b * CACHE_SIZE + f_idx] = (float)(flux / 32.0);
            self->energy_envelopes[b * CACHE_SIZE + f_idx] = (float)energy;
        }

        self->cache_write_ptr = (self->cache_write_ptr + 1) % CACHE_SIZE;
        if (self->cache_count < CACHE_SIZE) self->cache_count++;
    }

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

    // 1. Push ONLY NEW audio into cache
    analyzer_push_audio(self, y, len, sr);

    // 2. Linearize the 15.2s context from circular cache for processing
    int num_frames = self->cache_count;
    int read_ptr = (self->cache_write_ptr - num_frames + CACHE_SIZE) % CACHE_SIZE;

    BandAnalysis bands[MAX_BANDS];
    for (int b = 0; b < MAX_BANDS; b++) {
        bands[b].envelope = (float*)malloc(sizeof(float) * num_frames);
        bands[b].rolling_threshold = (float*)malloc(sizeof(float) * num_frames);
    }

    for (int b = 0; b < MAX_BANDS; b++) {
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

    // 4. Peak Detection on linearized buffer
    for (int b = 0; b < MAX_BANDS; b++) {
        float* env = bands[b].envelope;
        float* thresh = bands[b].rolling_threshold;
        int* temp_peaks = (int*)malloc(sizeof(int) * num_frames);
        int peak_count = 0;

        // Strategy 3: Internal Energy Percentile Gating
        float* energies = (float*)malloc(sizeof(float) * num_frames);
        float* sorted_energies = (float*)malloc(sizeof(float) * num_frames);
        for (int j = 0; j < num_frames; j++) {
            int f_idx = (read_ptr + j) % CACHE_SIZE;
            energies[j] = self->energy_envelopes[b * CACHE_SIZE + f_idx];
        }
        memcpy(sorted_energies, energies, sizeof(float) * num_frames);
        qsort(sorted_energies, num_frames, sizeof(float), compare_floats);

        for (int f = 1; f < num_frames - 1; f++) {
            if (env[f] > env[f-1] && env[f] > env[f+1] && env[f] > thresh[f]) {

                // Percentile Rank check
                float cur_e = energies[f];
                int count = 0;
                for (int k = 0; k < num_frames; k++) {
                    if (sorted_energies[k] < cur_e) count++;
                    else break;
                }
                float rank = (float)count / (float)num_frames;
                if (rank < 0.2f) continue; // Flat 20th Percentile Gate
                bool too_close = false;
                if (peak_count > 0 && f - temp_peaks[peak_count-1] < 200) {
                    if (env[f] > env[temp_peaks[peak_count-1]]) {
                        temp_peaks[peak_count-1] = f;
                    }
                    too_close = true;
                }

                if (!too_close) {
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
                        temp_peaks[peak_count++] = f;
                    }
                }
            }
        }
        bands[b].peaks = (int*)malloc(sizeof(int) * peak_count);
        memcpy(bands[b].peaks, temp_peaks, sizeof(int) * peak_count);
        bands[b].num_peaks = peak_count;
        free(temp_peaks);
        free(energies);
        free(sorted_energies);
    }

    // 5. Converging max_peak
    float global_max = 0;
    bool any_peak = false;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < bands[b].num_peaks; i++) {
            int p_idx = bands[b].peaks[i];
            float val = bands[b].envelope[p_idx];
            if (!any_peak || val > global_max) {
                global_max = val;
                any_peak = true;
            }
        }
    }
    if (any_peak && global_max > (float)self->max_peak) {
        self->max_peak = (double)global_max;
    }

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
            PeakResult pr;
            double time = (double)p_global * self->frame_duration_ms / 1000.0;
            if (analyzer_process_peak(self, p_idx, b, time, bands[b].envelope, num_frames, all_indices, total_peaks, &pr)) {
                if (self->snapshot_tails[b]) {
                    self->snapshot_tails[b]->p_idx = p_global;
                }
                if (result_out->peak_list.num_peaks < MAX_PEAKS_PER_CHUNK) {
                    pr.p_idx = p_global;
                    result_out->peak_list.peaks[result_out->peak_list.num_peaks++] = pr;
                }
            }
        }
    }

    analyzer_update_metrics(self, active_start_frame + 100, &result_out->metrics);

    // Cleanup linearized buffers
    for (int b = 0; b < MAX_BANDS; b++) {
        free(bands[b].envelope);
        free(bands[b].rolling_threshold);
        free(bands[b].peaks);
    }
    free(all_peaks_ref);
    free(all_indices);

    return 1;
}

#define N_FFT 2048
#define N_MELS 128

static double* create_mel_filterbank(int sr, int n_fft, int n_mels) {
    double* filters = (double*)malloc(sizeof(double) * n_mels * (n_fft / 2 + 1));
    memset(filters, 0, sizeof(double) * n_mels * (n_fft / 2 + 1));

    double f_min = 0;
    double f_max = sr / 2.0;

    static const double f_sp = 200.0 / 3.0;
    static const double min_log_hz = 1000.0;
    const double min_log_mel = (min_log_hz - 0.0) / f_sp;
    const double log_step = log(6.4) / 27.0;

    #define HZ_TO_MEL(hz) ((hz) < min_log_hz ? ((hz) - 0.0) / f_sp : min_log_mel + log((hz) / min_log_hz) / log_step)
    #define MEL_TO_HZ(mel) ((mel) < min_log_mel ? 0.0 + (mel) * f_sp : min_log_hz * exp(log_step * ((mel) - min_log_mel)))

    double mel_min = HZ_TO_MEL(f_min);
    double mel_max = HZ_TO_MEL(f_max);

    double* mel_points = (double*)malloc(sizeof(double) * (n_mels + 2));
    for (int i = 0; i < n_mels + 2; i++) {
        double mel = mel_min + i * (mel_max - mel_min) / (n_mels + 1);
        mel_points[i] = MEL_TO_HZ(mel);
    }

    int n_fft_bins = n_fft / 2 + 1;
    for (int j = 0; j < n_mels; j++) {
        double f_low = mel_points[j];
        double f_mid = mel_points[j+1];
        double f_high = mel_points[j+2];

        for (int i = 0; i < n_fft_bins; i++) {
            double f = i * (double)sr / n_fft;
            double weight = 0;
            if (f >= f_low && f <= f_mid) {
                weight = (f - f_low) / (f_mid - f_low);
            } else if (f >= f_mid && f <= f_high) {
                weight = (f_high - f) / (f_high - f_mid);
            }
            filters[j * n_fft_bins + i] = weight;
        }

        double enorm = 2.0 / (f_high - f_low);
        for (int i = 0; i < n_fft_bins; i++) {
            filters[j * n_fft_bins + i] *= enorm;
        }
    }

    free(mel_points);
    return filters;
}

int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out) {
    result_out->ratings = NULL;
    result_out->std_devs = NULL;
    result_out->contrasts = NULL;
    result_out->peak_stds = NULL;

    int hop_length = (int)(sr * 0.001);
    int n_fft = N_FFT;
    int n_mels = N_MELS;
    int num_frames = (len + hop_length - 1) / hop_length;

    result_out->num_frames = num_frames;
    result_out->times = (float*)malloc(sizeof(float) * num_frames);
    for (int i = 0; i < num_frames; i++) {
        result_out->times[i] = (float)i * (float)hop_length / (float)sr;
    }

    double* mel_filters = create_mel_filterbank(sr, n_fft, n_mels);

    double* window = (double*)malloc(sizeof(double) * n_fft);
    for (int i = 0; i < n_fft; i++) {
        window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (double)n_fft));
    }

    double* mel_spectrogram = (double*)malloc(sizeof(double) * n_mels * num_frames);
    memset(mel_spectrogram, 0, sizeof(double) * n_mels * num_frames);

    double* real = (double*)malloc(sizeof(double) * n_fft);
    double* imag = (double*)malloc(sizeof(double) * n_fft);

    for (int f = 0; f < num_frames; f++) {
        int start = f * hop_length - n_fft / 2;

        for (int i = 0; i < n_fft; i++) {
            int idx = start + i;
            if (idx < 0) {
                idx = -idx;
            } else if (idx >= len) {
                idx = 2 * len - 2 - idx;
            }
            
            if (idx >= 0 && idx < len) {
                real[i] = (double)y[idx] * window[i];
            } else {
                real[i] = 0;
            }
            imag[i] = 0;
        }

        fft(real, imag, n_fft);

        for (int m = 0; m < n_mels; m++) {
            double mel_val = 0;
            for (int i = 0; i < (n_fft / 2 + 1); i++) {
                double re = real[i];
                double im = imag[i];
                mel_val += (re * re + im * im) * mel_filters[m * (n_fft / 2 + 1) + i];
            }
            mel_spectrogram[m * num_frames + f] = mel_val;
        }
    }
    free(real);
    free(imag);
    free(window);
    free(mel_filters);

    for (int i = 0; i < n_mels * num_frames; i++) {
        double val = mel_spectrogram[i];
        if (val < 1e-10) val = 1e-10;
        mel_spectrogram[i] = 10.0 * log10(val);
    }
    double max_db = -1e20;
    for (int i = 0; i < n_mels * num_frames; i++) {
        if (mel_spectrogram[i] > max_db) max_db = mel_spectrogram[i];
    }
    double top_db = 80.0;
    for (int i = 0; i < n_mels * num_frames; i++) {
        mel_spectrogram[i] -= max_db;
        if (mel_spectrogram[i] < -top_db) mel_spectrogram[i] = -top_db;
    }

    int padding = n_fft / (2 * hop_length);

    for (int b = 0; b < MAX_BANDS; b++) {
        result_out->bands[b].envelope = (float*)malloc(sizeof(float) * num_frames);
        memset(result_out->bands[b].envelope, 0, sizeof(float) * num_frames);

        double* raw_flux = (double*)malloc(sizeof(double) * num_frames);
        for (int f = 1; f < num_frames; f++) {
            double flux = 0;
            for (int m = b * 32; m < (b + 1) * 32; m++) {
                double diff = mel_spectrogram[m * num_frames + f] - mel_spectrogram[m * num_frames + f - 1];
                if (diff > 0) flux += diff;
            }
            raw_flux[f] = flux / 32.0;
        }
        raw_flux[0] = 0;

        for (int f = 0; f < num_frames; f++) {
            int src_f = f - padding + 1;
            if (src_f < 1 || src_f >= num_frames) {
                result_out->bands[b].envelope[f] = 0.0f;
            } else {
                result_out->bands[b].envelope[f] = (float)raw_flux[src_f];
            }
        }
        free(raw_flux);

        result_out->bands[b].rolling_threshold = (float*)malloc(sizeof(float) * num_frames);
        double current_sum = 0;
        int window_size = 15000;
        for (int f = 0; f < num_frames; f++) {
            current_sum += (double)result_out->bands[b].envelope[f];
            if (f >= window_size) {
                current_sum -= (double)result_out->bands[b].envelope[f - window_size];
                result_out->bands[b].rolling_threshold[f] = (float)(current_sum / (double)window_size);
            } else {
                result_out->bands[b].rolling_threshold[f] = (float)(current_sum / (double)(f + 1));
            }
        }
    }

    for (int b = 0; b < MAX_BANDS; b++) {
        float* env = result_out->bands[b].envelope;
        float* thresh = result_out->bands[b].rolling_threshold;
        int* temp_peaks = (int*)malloc(sizeof(int) * num_frames);
        int peak_count = 0;

        for (int f = 1; f < num_frames - 1; f++) {
            if (env[f] > env[f-1] && env[f] > env[f+1] && env[f] > thresh[f]) {
                // Simplified SciPy-like find_peaks with distance and prominence
                bool too_close = false;
                if (peak_count > 0 && f - temp_peaks[peak_count-1] < 200) {
                    if (env[f] > env[temp_peaks[peak_count-1]]) {
                        temp_peaks[peak_count-1] = f;
                    }
                    too_close = true;
                }

                if (!too_close) {
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
                        temp_peaks[peak_count++] = f;
                    }
                }
            }
        }

        result_out->bands[b].peaks = (int*)malloc(sizeof(int) * peak_count);
        memcpy(result_out->bands[b].peaks, temp_peaks, sizeof(int) * peak_count);
        result_out->bands[b].num_peaks = peak_count;
        free(temp_peaks);
    }

    float global_max = 0;
    bool any_peak = false;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
            int p_idx = result_out->bands[b].peaks[i];
            float val = result_out->bands[b].envelope[p_idx];
            if (!any_peak || val > global_max) {
                global_max = val;
                any_peak = true;
            }
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
        for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
            all_valid_peaks[curr++] = result_out->bands[b].peaks[i];
        }
    }

    for (int f = 0; f < num_frames; f++) {
        for (int b = 0; b < MAX_BANDS; b++) {
            for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
                int p_idx = result_out->bands[b].peaks[i];
                if (p_idx == f) {
                    PeakResult pr;
                    analyzer_process_peak(analyzer, p_idx, b, result_out->times[f], result_out->bands[b].envelope, num_frames, all_valid_peaks, total_peaks, &pr);
                }
            }
        }

        AnalyzerMetrics m;
        analyzer_update_metrics(analyzer, f, &m);
        result_out->ratings[f] = m.rating;
        result_out->std_devs[f] = m.std_dev;
        result_out->contrasts[f] = m.contrast;
        result_out->peak_stds[f] = m.peak_std;
    }

    free(all_valid_peaks);
    analyzer_destroy(analyzer);
    return 1;
}

void analyzer_free_analysis(FullAnalysisResult* result) {
    if (result->times) free(result->times);
    for (int i = 0; i < MAX_BANDS; i++) {
        if (result->bands[i].envelope) free(result->bands[i].envelope);
        if (result->bands[i].rolling_threshold) free(result->bands[i].rolling_threshold);
        if (result->bands[i].peaks) free(result->bands[i].peaks);
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

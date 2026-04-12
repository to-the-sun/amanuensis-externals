#include "doubles_dsp.h"

// Simple Radix-2 FFT implementation
static void bit_reverse(double *real, double *imag, int n) {
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (i < j) {
            double temp_real = real[i];
            double temp_imag = imag[i];
            real[i] = real[j];
            imag[i] = imag[j];
            real[j] = temp_real;
            imag[j] = temp_imag;
        }
        int m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }
}

void fft_forward(double *real, double *imag, int n) {
    bit_reverse(real, imag, n);
    for (int s = 1; (1 << s) <= n; s++) {
        int m = 1 << s;
        double theta = -2.0 * M_PI / m;
        double w_real_step = cos(theta);
        double w_imag_step = sin(theta);
        for (int k = 0; k < n; k += m) {
            double w_real = 1.0;
            double w_imag = 0.0;
            for (int j = 0; j < m / 2; j++) {
                double u_real = real[k + j];
                double u_imag = imag[k + j];
                double v_real = real[k + j + m / 2] * w_real - imag[k + j + m / 2] * w_imag;
                double v_imag = real[k + j + m / 2] * w_imag + imag[k + j + m / 2] * w_real;
                real[k + j] = u_real + v_real;
                imag[k + j] = u_imag + v_imag;
                real[k + j + m / 2] = u_real - v_real;
                imag[k + j + m / 2] = u_imag - v_imag;
                double next_w_real = w_real * w_real_step - w_imag * w_imag_step;
                w_imag = w_real * w_imag_step + w_imag * w_real_step;
                w_real = next_w_real;
            }
        }
    }
}

static double hz_to_mel(double hz) {
    return 1127.0 * log(1.0 + hz / 700.0);
}

static double mel_to_hz(double mel) {
    return 700.0 * (exp(mel / 1127.0) - 1.0);
}

t_mel_filterbank *mel_filterbank_init(int num_filters, int fft_size, double sample_rate, double low_freq, double high_freq) {
    t_mel_filterbank *mfb = (t_mel_filterbank *)malloc(sizeof(t_mel_filterbank));
    mfb->num_filters = num_filters;
    mfb->fft_size = fft_size;
    mfb->sample_rate = sample_rate;

    double min_mel = hz_to_mel(low_freq);
    double max_mel = hz_to_mel(high_freq);
    double mel_step = (max_mel - min_mel) / (num_filters + 1);

    mfb->filter_starts = (int *)malloc(num_filters * sizeof(int));
    mfb->filter_ends = (int *)malloc(num_filters * sizeof(int));
    mfb->weights = (double **)malloc(num_filters * sizeof(double *));

    for (int i = 0; i < num_filters; i++) {
        double m_left = min_mel + i * mel_step;
        double m_center = min_mel + (i + 1) * mel_step;
        double m_right = min_mel + (i + 2) * mel_step;

        double f_left = mel_to_hz(m_left);
        double f_center = mel_to_hz(m_center);
        double f_right = mel_to_hz(m_right);

        int k_left = (int)floor((fft_size + 1) * f_left / sample_rate);
        int k_center = (int)floor((fft_size + 1) * f_center / sample_rate);
        int k_right = (int)floor((fft_size + 1) * f_right / sample_rate);

        mfb->filter_starts[i] = k_left;
        mfb->filter_ends[i] = k_right;
        int filter_len = k_right - k_left + 1;
        mfb->weights[i] = (double *)malloc(filter_len * sizeof(double));

        for (int k = k_left; k <= k_right; k++) {
            double freq = (double)k * sample_rate / (fft_size + 1);
            if (k < k_center) {
                mfb->weights[i][k - k_left] = (freq - f_left) / (f_center - f_left);
            } else {
                mfb->weights[i][k - k_left] = (f_right - freq) / (f_right - f_center);
            }
        }
    }
    return mfb;
}

void mel_filterbank_free(t_mel_filterbank *mfb) {
    if (mfb) {
        for (int i = 0; i < mfb->num_filters; i++) {
            free(mfb->weights[i]);
        }
        free(mfb->weights);
        free(mfb->filter_starts);
        free(mfb->filter_ends);
        free(mfb);
    }
}

void calculate_mfcc(double *audio_segment, int segment_size, int fft_size, t_mel_filterbank *mfb, int num_ceps, double *mfccs) {
    double *real = (double *)calloc(fft_size, sizeof(double));
    double *imag = (double *)calloc(fft_size, sizeof(double));

    // Apply Hamming window and zero pad
    for (int i = 0; i < segment_size; i++) {
        double window = 0.54 - 0.46 * cos(2.0 * M_PI * i / (segment_size - 1));
        real[i] = audio_segment[i] * window;
    }

    fft_forward(real, imag, fft_size);

    double *power_spec = (double *)malloc((fft_size / 2 + 1) * sizeof(double));
    for (int i = 0; i <= fft_size / 2; i++) {
        power_spec[i] = (real[i] * real[i] + imag[i] * imag[i]) / fft_size;
    }

    double *filter_energies = (double *)calloc(mfb->num_filters, sizeof(double));
    for (int i = 0; i < mfb->num_filters; i++) {
        for (int k = mfb->filter_starts[i]; k <= mfb->filter_ends[i]; k++) {
            if (k >= 0 && k <= fft_size / 2) {
                filter_energies[i] += power_spec[k] * mfb->weights[i][k - mfb->filter_starts[i]];
            }
        }
        if (filter_energies[i] < 1e-10) filter_energies[i] = 1e-10;
        filter_energies[i] = log(filter_energies[i]);
    }

    // DCT
    for (int i = 0; i < num_ceps; i++) {
        mfccs[i] = 0.0;
        for (int j = 0; j < mfb->num_filters; j++) {
            mfccs[i] += filter_energies[j] * cos(M_PI * i * (j + 0.5) / mfb->num_filters);
        }
    }

    free(real);
    free(imag);
    free(power_spec);
    free(filter_energies);
}

static double euclidean_distance(double *v1, double *v2, int n) {
    double dist = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = v1[i] - v2[i];
        dist += diff * diff;
    }
    return sqrt(dist);
}

t_dtw_path *dtw_calculate(double **ref_mfccs, int ref_len, double **subj_mfccs, int subj_len, int num_ceps) {
    if (ref_len == 0 || subj_len == 0) return NULL;

    // Use a Sakoe-Chiba band to restrict the search space and save memory/time.
    // Band width 'r' is 10% of the maximum length or a minimum of 50.
    int r = (int)(0.1 * (ref_len > subj_len ? ref_len : subj_len));
    if (r < 50) r = 50;

    double *cost_matrix = (double *)malloc(ref_len * subj_len * sizeof(double));
    if (!cost_matrix) return NULL;

    // Initialize with infinity
    for (int i = 0; i < ref_len * subj_len; i++) cost_matrix[i] = 1e30;

    cost_matrix[0] = euclidean_distance(ref_mfccs[0], subj_mfccs[0], num_ceps);

    for (int i = 0; i < ref_len; i++) {
        int j_start = (int)floor((double)i * subj_len / ref_len) - r;
        int j_end = (int)floor((double)i * subj_len / ref_len) + r;
        if (j_start < 0) j_start = 0;
        if (j_end >= subj_len) j_end = subj_len - 1;

        for (int j = j_start; j <= j_end; j++) {
            if (i == 0 && j == 0) continue;

            double dist = euclidean_distance(ref_mfccs[i], subj_mfccs[j], num_ceps);
            double v1 = (i > 0) ? cost_matrix[(i - 1) * subj_len + j] : 1e30;
            double v2 = (j > 0) ? cost_matrix[i * subj_len + (j - 1)] : 1e30;
            double v3 = (i > 0 && j > 0) ? cost_matrix[(i - 1) * subj_len + (j - 1)] : 1e30;

            double min_prev = v1;
            if (v2 < min_prev) min_prev = v2;
            if (v3 < min_prev) min_prev = v3;

            if (min_prev < 1e30) {
                cost_matrix[i * subj_len + j] = dist + min_prev;
            }
        }
    }

    // Backtracking
    t_dtw_point *temp_path = (t_dtw_point *)malloc((ref_len + subj_len) * sizeof(t_dtw_point));
    if (!temp_path) {
        free(cost_matrix);
        return NULL;
    }
    int path_len = 0;
    int i = ref_len - 1;
    int j = subj_len - 1;

    while (i > 0 || j > 0) {
        temp_path[path_len].ref_idx = i;
        temp_path[path_len].subj_idx = j;
        path_len++;

        double v1 = (i > 0) ? cost_matrix[(i - 1) * subj_len + j] : 1e31;
        double v2 = (j > 0) ? cost_matrix[i * subj_len + (j - 1)] : 1e31;
        double v3 = (i > 0 && j > 0) ? cost_matrix[(i - 1) * subj_len + (j - 1)] : 1e31;

        if (v3 <= v1 && v3 <= v2) {
            i--; j--;
        } else if (v1 < v2) {
            i--;
        } else {
            j--;
        }

        // Safety break for out of band issues
        if (v1 >= 1e30 && v2 >= 1e30 && v3 >= 1e30) break;
    }
    temp_path[path_len].ref_idx = 0;
    temp_path[path_len].subj_idx = 0;
    path_len++;

    t_dtw_path *path = (t_dtw_path *)malloc(sizeof(t_dtw_path));
    if (path) {
        path->length = path_len;
        path->points = (t_dtw_point *)malloc(path_len * sizeof(t_dtw_point));
        if (path->points) {
            for (int k = 0; k < path_len; k++) {
                path->points[k] = temp_path[path_len - 1 - k];
            }
        } else {
            free(path);
            path = NULL;
        }
    }

    free(temp_path);
    free(cost_matrix);
    return path;
}

void dtw_path_free(t_dtw_path *path) {
    if (path) {
        free(path->points);
        free(path);
    }
}

void wsola_process(float *ref_samples, long long ref_frames, float *subj_samples, long long subj_frames, float *dest_samples, t_dtw_path *path, int hop_size, int win_size) {
    int search_range = win_size / 2;
    float *win = (float *)malloc(win_size * sizeof(float));
    float *ola_norm = (float *)calloc(ref_frames, sizeof(float));
    for (int i = 0; i < win_size; i++) {
        win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (win_size - 1)));
    }

    // Pre-calculate mapping from ref_mfcc_idx to subj_mfcc_idx
    // Find the max ref_idx in the path
    int max_ref_idx = 0;
    for (int i = 0; i < path->length; i++) {
        if (path->points[i].ref_idx > max_ref_idx) max_ref_idx = path->points[i].ref_idx;
    }

    double *ref_to_subj = (double *)malloc((max_ref_idx + 1) * sizeof(double));
    int *ref_counts = (int *)calloc((max_ref_idx + 1), sizeof(int));
    for (int i = 0; i <= max_ref_idx; i++) ref_to_subj[i] = 0;

    for (int i = 0; i < path->length; i++) {
        ref_to_subj[path->points[i].ref_idx] += path->points[i].subj_idx;
        ref_counts[path->points[i].ref_idx]++;
    }
    for (int i = 0; i <= max_ref_idx; i++) {
        if (ref_counts[i] > 0) ref_to_subj[i] /= ref_counts[i];
        else if (i > 0) ref_to_subj[i] = ref_to_subj[i-1];
    }
    free(ref_counts);

    // Initialize dest_samples with 0
    memset(dest_samples, 0, ref_frames * sizeof(float));

    long long out_pos = 0;
    long long last_actual_subj_pos = -hop_size;

    // We step through the output (which matches ref_frames) in hop_size increments
    // We ensure the loop covers the entire ref_frames by allowing out_pos to go slightly further
    while (out_pos < ref_frames - hop_size) {
        int ref_mfcc_idx = (int)(out_pos / hop_size);
        if (ref_mfcc_idx > max_ref_idx) ref_mfcc_idx = max_ref_idx;

        double target_subj_pos = ref_to_subj[ref_mfcc_idx] * hop_size;

        // WSOLA refinement: search for best cross-correlation around target_subj_pos
        // to maintain phase continuity with the previous grain.
        long long natural_subj_pos = last_actual_subj_pos + hop_size;
        long long search_start = (long long)target_subj_pos - search_range;
        long long search_end = (long long)target_subj_pos + search_range;

        if (search_start < 0) search_start = 0;
        if (search_end + win_size > subj_frames) search_end = subj_frames - win_size;

        long long best_subj_pos = (long long)target_subj_pos;
        if (out_pos > 0) {
            double max_corr = -1e20;
            for (long long s = search_start; s <= search_end; s++) {
                double corr = 0;
                for (int i = 0; i < search_range; i++) {
                    // Correlate the current candidate grain with the expected natural progression
                    if (natural_subj_pos + i < subj_frames && s + i < subj_frames) {
                        corr += (double)subj_samples[natural_subj_pos + i] * subj_samples[s + i];
                    }
                }
                if (corr > max_corr) {
                    max_corr = corr;
                    best_subj_pos = s;
                }
            }
        }

        last_actual_subj_pos = best_subj_pos;

        // OLA
        for (int i = 0; i < win_size; i++) {
            if (out_pos + i < ref_frames && best_subj_pos + i < subj_frames) {
                dest_samples[out_pos + i] += subj_samples[best_subj_pos + i] * win[i];
                ola_norm[out_pos + i] += win[i];
            }
        }

        out_pos += hop_size;
    }

    // Handle the tail: ensure the last part of the destination buffer is filled
    // if out_pos didn't reach the end.
    if (out_pos < ref_frames) {
        int ref_mfcc_idx = (int)(out_pos / hop_size);
        if (ref_mfcc_idx > max_ref_idx) ref_mfcc_idx = max_ref_idx;
        double target_subj_pos = ref_to_subj[ref_mfcc_idx] * hop_size;
        long long best_subj_pos = (long long)target_subj_pos;
        if (best_subj_pos + (ref_frames - out_pos) > subj_frames) {
            best_subj_pos = subj_frames - (ref_frames - out_pos);
        }
        if (best_subj_pos < 0) best_subj_pos = 0;

        for (long long i = out_pos; i < ref_frames; i++) {
            long long s_idx = best_subj_pos + (i - out_pos);
            if (s_idx < subj_frames) {
                // For the tail, we just copy or use a simple OLA if it's within a window
                dest_samples[i] = subj_samples[s_idx];
                ola_norm[i] = 1.0;
            } else {
                // If we run out of subject, fill with silence to ensure no holes in norm
                dest_samples[i] = 0;
                ola_norm[i] = 1.0;
            }
        }
    }

    // Normalize OLA
    for (long long i = 0; i < ref_frames; i++) {
        if (ola_norm[i] > 1e-6) {
            dest_samples[i] /= ola_norm[i];
        }
    }

    free(ref_to_subj);
    free(win);
    free(ola_norm);
}

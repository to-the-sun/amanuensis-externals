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
    if (!mfb) return NULL;
    mfb->num_filters = num_filters;
    mfb->fft_size = fft_size;
    mfb->sample_rate = sample_rate;

    double min_mel = hz_to_mel(low_freq);
    double max_mel = hz_to_mel(high_freq);
    double mel_step = (max_mel - min_mel) / (num_filters + 1);

    mfb->filter_starts = (int *)malloc(num_filters * sizeof(int));
    mfb->filter_ends = (int *)malloc(num_filters * sizeof(int));
    mfb->weights = (double **)malloc(num_filters * sizeof(double *));
    if (!mfb->filter_starts || !mfb->filter_ends || !mfb->weights) {
        if (mfb->filter_starts) free(mfb->filter_starts);
        if (mfb->filter_ends) free(mfb->filter_ends);
        if (mfb->weights) free(mfb->weights);
        free(mfb);
        return NULL;
    }

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

void normalize_mfccs(double **mfccs, int num_frames, int num_ceps) {
    for (int i = 0; i < num_ceps; i++) {
        double sum = 0;
        for (int j = 0; j < num_frames; j++) sum += mfccs[j][i];
        double mean = sum / num_frames;
        for (int j = 0; j < num_frames; j++) mfccs[j][i] -= mean;
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

void detect_transients(float *samples, long long num_frames, int win_size, int hop_size, double *transients) {
    int num_windows = (int)((num_frames - win_size) / hop_size) + 1;
    double prev_energy = 0;

    for (int i = 0; i < num_windows; i++) {
        double energy = 0;
        for (int j = 0; j < win_size; j++) {
            float s = samples[i * hop_size + j];
            energy += s * s;
        }
        energy = sqrt(energy / win_size);

        // Simple onset detection based on energy increase
        if (i > 0 && energy > prev_energy * 2.0 && energy > 0.01) {
            transients[i] = 1.0;
        } else {
            transients[i] = 0.0;
        }
        prev_energy = energy;
    }
}

static double euclidean_distance(double *v1, double *v2, int n) {
    double dist = 0.0;
    // Skip the first coefficient (C0/Energy) to make alignment invariant to volume differences
    for (int i = 1; i < n; i++) {
        double diff = v1[i] - v2[i];
        dist += diff * diff;
    }
    return sqrt(dist);
}

t_dtw_path *dtw_calculate(double **ref_mfccs, int ref_len, double **subj_mfccs, int subj_len, int num_ceps, double *ref_transients, double *subj_transients) {
    if (ref_len == 0 || subj_len == 0) return NULL;

    int r = (int)(0.2 * (ref_len > subj_len ? ref_len : subj_len));
    if (r < 50) r = 50;

    double *cost_matrix = (double *)malloc(ref_len * subj_len * sizeof(double));
    if (!cost_matrix) return NULL;

    for (int i = 0; i < ref_len * subj_len; i++) cost_matrix[i] = 1e30;

    cost_matrix[0] = euclidean_distance(ref_mfccs[0], subj_mfccs[0], num_ceps);

    // Track consecutive horizontal/vertical steps to enforce slope constraints
    // For simplicity in a 2D matrix, we'll just use a basic DTW and handle constraints
    // during the cost calculation or by using a more complex state-space DTW.
    // Let's refine the cost to penalize staying too long on one frame.

    for (int i = 0; i < ref_len; i++) {
        int j_start = (int)floor((double)i * subj_len / ref_len) - r;
        int j_end = (int)floor((double)i * subj_len / ref_len) + r;
        if (j_start < 0) j_start = 0;
        if (j_end >= subj_len) j_end = subj_len - 1;

        for (int j = j_start; j <= j_end; j++) {
            if (i == 0 && j == 0) continue;

            double dist = euclidean_distance(ref_mfccs[i], subj_mfccs[j], num_ceps);

            // Penalty for stretching/compressing transients
            double transient_penalty = 1.0;
            if (ref_transients[i] > 0.5 || subj_transients[j] > 0.5) {
                transient_penalty = 5.0; // Make non-diagonal moves more expensive during transients
            }

            double v_diag = (i > 0 && j > 0) ? cost_matrix[(i - 1) * subj_len + (j - 1)] : 1e30;
            double v_horz = (j > 0) ? cost_matrix[i * subj_len + (j - 1)] : 1e30;
            double v_vert = (i > 0) ? cost_matrix[(i - 1) * subj_len + j] : 1e30;

            // Apply transient penalty to non-diagonal moves
            v_horz += dist * transient_penalty;
            v_vert += dist * transient_penalty;
            v_diag += dist;

            double min_prev = v_diag;
            if (v_horz < min_prev) min_prev = v_horz;
            if (v_vert < min_prev) min_prev = v_vert;

            if (min_prev < 1e30) {
                cost_matrix[i * subj_len + j] = min_prev;
            }
        }
    }

    // Backtracking with slope constraint (don't stay on same frame too long)
    t_dtw_point *temp_path = (t_dtw_point *)malloc((ref_len + subj_len) * sizeof(t_dtw_point));
    if (!temp_path) {
        free(cost_matrix);
        return NULL;
    }
    int path_len = 0;
    int i = ref_len - 1;
    int j = subj_len - 1;
    int consecutive_horz = 0;
    int consecutive_vert = 0;

    while (i > 0 || j > 0) {
        temp_path[path_len].ref_idx = i;
        temp_path[path_len].subj_idx = j;
        path_len++;

        double v_diag = (i > 0 && j > 0) ? cost_matrix[(i - 1) * subj_len + (j - 1)] : 1e31;
        double v_horz = (j > 0) ? cost_matrix[i * subj_len + (j - 1)] : 1e31;
        double v_vert = (i > 0) ? cost_matrix[(i - 1) * subj_len + j] : 1e31;

        // Slope constraint: If we've moved horizontally 2 times, force a diagonal or vertical move
        if (consecutive_horz >= 2) v_horz = 1e32;
        if (consecutive_vert >= 2) v_vert = 1e32;

        if (v_diag <= v_horz && v_diag <= v_vert) {
            i--; j--;
            consecutive_horz = 0;
            consecutive_vert = 0;
        } else if (v_horz < v_vert) {
            j--;
            consecutive_horz++;
            consecutive_vert = 0;
        } else {
            i--;
            consecutive_vert++;
            consecutive_horz = 0;
        }

        if (v_diag >= 1e30 && v_horz >= 1e30 && v_vert >= 1e30) break;
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

double *dtw_path_to_mapping(t_dtw_path *path, int *out_mapping_len) {
    if (!path || path->length == 0) return NULL;

    int max_ref_idx = 0;
    for (int i = 0; i < path->length; i++) {
        if (path->points[i].ref_idx > max_ref_idx) max_ref_idx = path->points[i].ref_idx;
    }

    int mapping_len = max_ref_idx + 1;
    double *mapping = (double *)malloc(mapping_len * sizeof(double));
    int *counts = (int *)calloc(mapping_len, sizeof(int));

    for (int i = 0; i < mapping_len; i++) mapping[i] = 0;

    for (int i = 0; i < path->length; i++) {
        mapping[path->points[i].ref_idx] += path->points[i].subj_idx;
        counts[path->points[i].ref_idx]++;
    }

    for (int i = 0; i < mapping_len; i++) {
        if (counts[i] > 0) mapping[i] /= counts[i];
        else if (i > 0) mapping[i] = mapping[i - 1];
    }

    free(counts);
    if (out_mapping_len) *out_mapping_len = mapping_len;
    return mapping;
}

void wsola_process(float *ref_samples, long long ref_frames, float *subj_samples, long long subj_frames, float *dest_samples, t_dtw_path *path, int hop_size, int win_size) {
    int search_range = win_size / 2;
    float *win = (float *)malloc(win_size * sizeof(float));
    float *ola_norm = (float *)calloc(ref_frames, sizeof(float));
    for (int i = 0; i < win_size; i++) {
        win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (win_size - 1)));
    }

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

    memset(dest_samples, 0, ref_frames * sizeof(float));

    long long out_pos = 0;
    long long last_actual_subj_pos = -hop_size;

    while (out_pos < ref_frames - hop_size) {
        int ref_mfcc_idx = (int)(out_pos / hop_size);
        if (ref_mfcc_idx > max_ref_idx) ref_mfcc_idx = max_ref_idx;

        double target_subj_pos = ref_to_subj[ref_mfcc_idx] * hop_size;

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
                double energy_s = 0;
                for (int i = 0; i < search_range; i++) {
                    if (natural_subj_pos + i < subj_frames && s + i < subj_frames) {
                        double val_n = subj_samples[natural_subj_pos + i];
                        double val_s = subj_samples[s + i];
                        corr += val_n * val_s;
                        energy_s += val_s * val_s;
                    }
                }

                double dist_to_target = (double)abs((int)(s - target_subj_pos)) / search_range;
                corr = corr - 0.01 * dist_to_target * energy_s;

                if (corr > max_corr) {
                    max_corr = corr;
                    best_subj_pos = s;
                }
            }
        }

        last_actual_subj_pos = best_subj_pos;

        for (int i = 0; i < win_size; i++) {
            if (out_pos + i < ref_frames && best_subj_pos + i < subj_frames) {
                dest_samples[out_pos + i] += subj_samples[best_subj_pos + i] * win[i];
                ola_norm[out_pos + i] += win[i];
            }
        }

        out_pos += hop_size;
    }

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
                dest_samples[i] = subj_samples[s_idx];
                ola_norm[i] = 1.0;
            } else {
                dest_samples[i] = 0;
                ola_norm[i] = 1.0;
            }
        }
    }

    for (long long i = 0; i < ref_frames; i++) {
        if (ola_norm[i] > 1e-6) {
            dest_samples[i] /= ola_norm[i];
        } else if (i < ref_frames) {
            dest_samples[i] = 0;
        }
    }

    free(ref_to_subj);
    free(win);
    free(ola_norm);
}

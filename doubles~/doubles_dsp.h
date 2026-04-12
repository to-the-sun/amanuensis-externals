#ifndef DOUBLES_DSP_H
#define DOUBLES_DSP_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct _mel_filterbank {
    int num_filters;
    int fft_size;
    double sample_rate;
    int *filter_starts;
    int *filter_ends;
    double **weights;
} t_mel_filterbank;

// FFT functions
void fft_init();
void fft_forward(double *real, double *imag, int n);

// MFCC functions
t_mel_filterbank *mel_filterbank_init(int num_filters, int fft_size, double sample_rate, double low_freq, double high_freq);
void mel_filterbank_free(t_mel_filterbank *mfb);
void normalize_mfccs(double **mfccs, int num_frames, int num_ceps);
void calculate_mfcc(double *audio_segment, int segment_size, int fft_size, t_mel_filterbank *mfb, int num_ceps, double *mfccs);

// DTW functions
typedef struct _dtw_point {
    int ref_idx;
    int subj_idx;
} t_dtw_point;

typedef struct _dtw_path {
    t_dtw_point *points;
    int length;
} t_dtw_path;

t_dtw_path *dtw_calculate(double **ref_mfccs, int ref_len, double **subj_mfccs, int subj_len, int num_ceps);
void dtw_path_free(t_dtw_path *path);

// WSOLA functions
void wsola_process(float *ref_samples, long long ref_frames, float *subj_samples, long long subj_frames, float *dest_samples, t_dtw_path *path, int hop_size, int win_size);

#endif

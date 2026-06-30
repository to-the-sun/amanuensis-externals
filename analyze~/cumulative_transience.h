#ifndef CUMULATIVE_TRANSIENCE_H
#define CUMULATIVE_TRANSIENCE_H

#include <stdint.h>
#include <stdbool.h>

#define BUFFER_LEN 5001
#define MAX_BANDS 4
#define MAX_QUALIFIERS 256
#define MAX_PEAK_HISTORY 8192
#define MAX_EVENTS 32768

typedef struct {
    double ms;
    double val;
} Qualifier;

typedef struct {
    int p_idx;
    int band_idx;
    double time;
    double peak_val;
    double total_score;
    int num_qualifiers;
    Qualifier qualifiers[MAX_QUALIFIERS];
    double snapshot[BUFFER_LEN];
} PeakResult;

typedef struct SnapshotEntry {
    int p_idx;
    double snapshot[BUFFER_LEN];
    struct SnapshotEntry* next;
} SnapshotEntry;

typedef struct {
    double std_dev;
    double mean;
    double contrast;
    double peak_std;
    double rating;
    bool buffer_updated;
    double highest_peak_ms;
    bool highest_peak_valid;
    double min_score_seen;
    double max_score_seen;
} AnalyzerMetrics;

#define MAX_PEAKS_PER_CHUNK 16

typedef struct {
    PeakResult peaks[MAX_PEAKS_PER_CHUNK];
    int num_peaks;
} PeakResultList;

typedef struct {
    PeakResultList peak_list;
    AnalyzerMetrics metrics;
} ChunkAnalysisResult;

typedef struct {
    double accumulated_buffer[BUFFER_LEN];
    double buffer_times[BUFFER_LEN];
    double max_peak;

    double min_score_seen;
    double max_score_seen;
    double total_score_sum;
    int score_count;
    double last_score_avg;

    // Peak history
    double peak_history[MAX_PEAK_HISTORY];
    int peak_history_count;

    // Snapshots tracking (queue per band)
    SnapshotEntry* snapshot_heads[MAX_BANDS];
    SnapshotEntry* snapshot_tails[MAX_BANDS];

    double frame_duration_ms;

    // Incremental Cache State
    double* mel_spectrogram;    // Mel bands cache
    float* flux_envelopes;      // Flux cache per band
    float* energy_envelopes;    // Linear energy cache per band
    double* mel_filters;        // Pre-calculated filters
    double* fft_window;         // Pre-calculated window
    int cache_write_ptr;
    int cache_count;
    int sample_rate;
} TransientAnalyzer;

TransientAnalyzer* analyzer_create(double max_peak_value);
void analyzer_destroy(TransientAnalyzer* self);
void analyzer_set_sample_rate(TransientAnalyzer* self, int sr);

int analyzer_process_peak(TransientAnalyzer* self,
                          int p_idx,
                          int band_idx,
                          double time,
                          const float* env_ptr,
                          int env_len,
                          const int* all_valid_peak_indices,
                          int all_valid_count,
                          PeakResult* result_out);

void analyzer_update_metrics(TransientAnalyzer* self, int frame, AnalyzerMetrics* metrics_out);
double* analyzer_get_buffer(TransientAnalyzer* self);

int analyzer_analyze_chunk(TransientAnalyzer* self,
                           const float* y,
                           int len,
                           int sr,
                           int buffer_start_frame,
                           int active_start_frame,
                           ChunkAnalysisResult* result_out);

void analyzer_push_audio(TransientAnalyzer* self, const float* y, int len, int sr);

// Full analysis structures
typedef struct {
    float* envelope;
    float* rolling_threshold;
    int* peaks;
    int num_peaks;
} BandAnalysis;

typedef struct {
    float* times;
    int num_frames;
    float max_peak_value;
    BandAnalysis bands[MAX_BANDS];

    // Batch analysis metrics history
    double* ratings;
    double* std_devs;
    double* contrasts;
    double* peak_stds;
} FullAnalysisResult;

int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out);
int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out);
void analyzer_free_analysis(FullAnalysisResult* result);
void analyzer_debug_mel_filters(int sr, int n_fft, int n_mels, double* filters_out);

#endif

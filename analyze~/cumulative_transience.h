#ifndef CUMULATIVE_TRANSIENCE_H
#define CUMULATIVE_TRANSIENCE_H

#include <stdbool.h>

#define MAX_BANDS 4
#define BUFFER_LEN 15000
#define MAX_QUALIFIERS 100
#define MAX_EVENTS 1024
#define MAX_PEAK_HISTORY 1024

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

typedef struct {
    double std_dev;
    double mean;
    double contrast;
    double rating;
    bool buffer_updated;
    double min_score_seen;
    double max_score_seen;
    bool highest_peak_valid;
    double highest_peak_ms;
    double peak_std;
    double rolling_score;
} AnalyzerMetrics;

typedef struct {
    int frame;
    int type; // 0: ADD, 1: REMOVE
    double score;
} ScoreEvent;

typedef struct SnapshotEntry {
    int p_idx;
    double snapshot[BUFFER_LEN];
    struct SnapshotEntry* next;
} SnapshotEntry;

typedef struct {
    double max_peak;
    double buffer_times[BUFFER_LEN];
    double min_score_seen;
    double max_score_seen;
    double last_score_avg;
    SnapshotEntry* snapshot_heads[MAX_BANDS];
    SnapshotEntry* snapshot_tails[MAX_BANDS];
    double accumulated_buffer[BUFFER_LEN];
    double total_score_sum;
    int score_count;
    int event_count;
    ScoreEvent upcoming_events[MAX_EVENTS];
    int event_read_ptr;
    int current_window_count;
    double current_window_scores[MAX_EVENTS];
    int peak_history_count;
    double peak_history[MAX_PEAK_HISTORY];
} TransientAnalyzer;

typedef struct {
    float* envelope;
    float* rolling_threshold;
    int* peaks;
    int num_peaks;
} BandAnalysis;

typedef struct {
    float* times;
    int num_frames;
    BandAnalysis bands[MAX_BANDS];
    float max_peak_value;
    double* rolling_scores;
    double* ratings;
    double* std_devs;
    double* contrasts;
    double* peak_stds;
} FullAnalysisResult;

// Function prototypes
TransientAnalyzer* analyzer_create(double max_peak_value);
void analyzer_destroy(TransientAnalyzer* self);
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
int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out);
int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out);
void analyzer_free_analysis(FullAnalysisResult* result);
void analyzer_debug_mel_filters(int sr, int n_fft, int n_mels, double* filters_out);

#endif // CUMULATIVE_TRANSIENCE_H

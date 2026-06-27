# Analysis Discrepancy Report: `analyze~` vs. Python Ground Truth

## Executive Summary
Discrepancies in the output of the `analyze~` Max object compared to its Python reference implementation are likely caused by differences in how the real-time audio stream is windowed and normalized. While the underlying C algorithm is identical, the execution environment in Max introduces constraints—specifically window size and dynamic normalization—that deviate from the batch-processing nature of the Python script.

## Technical Speculations

### 1. Context Window Mismatch
The `analyze~` object currently extracts a **2-second** context window (`analysis_samples`) from its circular buffer to perform spectral analysis. However, the `analyzer_process_peak` function in `cumulative_transience.c` implements a **5-second** resonance lookback:
```c
int start = p_idx - 5000; // 5000ms = 5 seconds
// ...
if (s_idx >= p_idx - 5000 && s_idx <= p_idx - 99) {
    // Calculate resonance...
}
```
Because the real-time analysis window is shorter than the algorithm's internal lookback buffer, the analyzer is "blind" to 60% of the historical context required to calculate accurate resonance scores. This results in significantly lower `total_score` and `rating` values compared to the Python ground truth, which processes the entire file with full context.

### 2. Volatile Spectrogram Normalization
In `analyzer_analyze_audio`, the Mel spectrogram is normalized relative to the peak decibel level found **within the current window**:
```c
double max_db = -1e20;
for (int i = 0; i < n_mels * num_frames; i++) {
    if (mel_spectrogram[i] > max_db) max_db = mel_spectrogram[i];
}
// ...
mel_spectrogram[i] -= max_db;
```
In a real-time sliding window, this `max_db` value fluctuates every 100ms as new audio enters and old audio leaves. This causes the resulting flux envelopes to "breathe" or shift their scale relative to one another. The Python ground truth likely calculates a single `max_db` for the entire recording, ensuring a stable reference for all frames.

### 3. Incorrect `max_peak` Reference
The `TransientAnalyzer` state is initialized in Max with a hardcoded `max_peak` of `1.0`:
```c
x->analyzer = analyzer_create(1.0);
```
In batch processing (Python), the analyzer first scans the entire file to find the true global maximum flux and uses that value for `max_peak`. In `cumulative_transience.c`, this value is used to normalize snapshots:
```c
double normalization = (self->max_peak > 0) ? (result_out->peak_val / self->max_peak) : 1.0;
```
If the actual maximum flux in the audio is significantly different from `1.0` (e.g., `15.0`), the real-time analyzer will apply a drastically different scaling factor to its internal `accumulated_buffer`, leading to divergent `std_dev` and `contrast` metrics.

### 4. Fragmented Peak History
When calling `analyzer_process_peak`, the Max object only provides the list of peaks found within the current 2-second analysis window. The resonance algorithm expects a list of *all* relevant peaks in the 5-second lookback period. Even if the window size were increased, the real-time implementation only triggers analysis on "new" peaks. It does not maintain a persistent history of peaks across window boundaries to pass into the `all_valid_peak_indices` parameter, further starving the resonance calculation of necessary data points.

## Proposed Remediation Strategy
1. **Increase Buffer Context**: Extend the real-time analysis window to at least 6 seconds to accommodate the 5-second lookback.
2. **Implement Stable Normalization**: Replace the per-window `max_db` normalization with either a fixed reference level or a slow-moving peak tracker.
3. **Dynamic `max_peak` Estimation**: Update the `max_peak` value in the analyzer state dynamically as new, higher flux peaks are discovered.
4. **Persistent Peak Tracker**: Maintain a historical list of peak indices in the `t_analyze` struct to ensure the `analyzer_process_peak` function has access to a complete 5-second peak history.

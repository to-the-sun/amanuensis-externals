# Spectral Analysis Pipeline Report: Data Flow and Orchestration

This report clarifies the architectural relationship between the core C transience algorithm (`cumulative_transience.c`) and its host environments (Max and Python), specifically addressing the flow of spectral data and peak detection.

## 1. Where does the Spectral Analysis occur?

**The spectral analysis (FFT, Mel-filtering, and Spectral Flux calculation) occurs entirely within the C core.**

Specifically, the function `analyzer_analyze_audio` in `cumulative_transience.c` handles the following:
1.  **Windowing & FFT**: Converts raw time-domain audio into the frequency domain.
2.  **Mel-Filtering**: Aggregates FFT bins into 128 Mel-spaced bands.
3.  **Spectral Flux**: Calculates the rate of change in energy for each band.
4.  **Band Summation**: Groups the 128 Mel bands into the 4 analysis bands (0-31, 32-63, 64-95, 96-127).
5.  **Peak Detection**: Applies a rolling threshold and prominence check to the flux envelopes to identify transient candidates.

The results are stored in a `FullAnalysisResult` structure, which contains the raw flux envelopes and a list of detected peak indices for each band.

## 2. Why is there a "Back and Forth" between C and the Host?

The user asked: *Are you saying that the C detects the peaks, then hands it back to Python/Max, then Python/Max sorts them... then hands them back to C for the rest of the analysis?*

**Yes.** This "orchestration" pattern exists because of the difference between **Detection** and **Resonance Analysis**:

*   **Detection (Stateless/Local)**: Identifying a peak requires looking at a short neighborhood of audio (e.g., 200ms). This is handled by `analyzer_analyze_audio`.
*   **Resonance (Stateful/Global)**: The resonance score of a peak depends on the *history* of all previous peaks across all bands over a 5-second window. This is handled by `analyzer_process_peak`.

### The Rationale for Host Orchestration
By handing the detected peaks back to the host (Max or Python), the system gains necessary flexibility for different use cases:

1.  **Real-Time Windowing (Max)**: In Max, we analyze a 6-second sliding window every 100ms. The host must track which peaks have already been processed in previous windows to avoid "double-counting" them as they slide through.
2.  **Timeline Synchronization**: The host is responsible for mapping "local" peak indices (e.g., "Frame 500 in this 6-second window") to "global" timestamps (e.g., "Frame 45200 in the total audio stream").
3.  **Selective Processing**: The host could, in theory, choose to ignore certain peaks or prioritize specific bands before passing them to the resonance engine.

## 3. Why the Difference in Inter-band Sorting?

The discrepancy in inter-band processing order (documented in `ANALYSIS_INPUT_DIFFERENCES.md`) arises from how the host loops through the `FullAnalysisResult`.

### Python (Chronological)
The Python implementation (specifically in `ct_extension.pyx`) collects all peaks from all four bands, puts them into a single list of tuples `(p_idx, band_idx)`, and **sorts them by `p_idx`**.
- **Result**: Peaks are processed in strict temporal order, regardless of which band they belong to. A peak at 100ms in Band 3 will be processed before a peak at 200ms in Band 0.

### Max (Band-Sequential)
The Max external iterates through the bands one by one in its worker task:
```c
for (int b = 0; b < MAX_BANDS; b++) {
    for (int i = 0; i < result.bands[b].num_peaks; i++) {
        // Process peak...
    }
}
```
- **Result**: All peaks for Band 0 are processed, then all for Band 1, and so on. If Band 0 has a peak at 500ms and Band 1 has a peak at 100ms, the 500ms peak is processed *first*.
- **Impact**: Because the resonance algorithm is additive (each processed peak adds energy to the cumulative buffer), the Band 0 peak fails to "see" the resonance energy of the Band 1 peak that technically occurred before it.

## 4. Why doesn't C handle the whole process?

C **can** handle the whole process, and it does so in `analyzer_batch_analyze`. This function:
1.  Calls `analyzer_analyze_audio` to find all peaks.
2.  Internally sorts all peaks chronologically.
3.  Calls `analyzer_process_peak` for each.

**However, `analyzer_batch_analyze` is only suitable for offline, whole-file analysis.**

In a real-time environment like **Max**, we cannot use a batch function because the "whole file" doesn't exist yet—it's a continuous stream. The host (`analyze~.c`) must manage the circular buffer, the background threading, and the persistence of the `TransientAnalyzer` state across successive 100ms analysis cycles.

The "back and forth" is the price paid for supporting both offline batch processing (Python) and real-time streaming (Max) using the same underlying numerical core.

## 5. Proposed Change: Unified 15s Sliding Window

The user asked: *Does Python send the entire song of audio for FFT at once?*
**Yes.** In the current Python implementation, the entire file is loaded and passed to `analyzer_batch_analyze` (or `analyzer_analyze_audio`), which performs the STFT on the full duration in one pass.

The user asked: *What would be the effect of both pipelines only sending a smaller rolling window (e.g., 15s + 200ms lookahead)?*

Switching both Python and Max to a 15s sliding window would "harmonize" the two environments. The primary effects would be:
- **Consistent Normalization**: Both would experience the same spectral environment, meaning `max_peak` would converge similarly.
- **Identical Artifacts**: Any windowing artifacts (like STFT edge padding) would be identical in both versions, rather than Python being "clean" and Max having edge ripples.
- **Computational Efficiency (Python)**: For very long files, memory usage would drop significantly as Python would no longer need to hold the full Mel-spectrogram of the entire song in RAM.

### Speculative Impact on `ANALYSIS_INPUT_DIFFERENCES.md`

If we moved to a unified 15s sliding window with a 200ms lookahead, here is how the documented discrepancies would change:

1.  **Global vs. Converging `max_peak`**:
    - *Impact*: **Reduced.** While Python would still technically "see" the future if it looks ahead, the 15s limit ensures that early normalization is driven by local energy rather than a distant global maximum. This makes Python's behavior much closer to Max's real-time convergence.
2.  **Peak Suppression and Minimum Distance**:
    - *Impact*: **Eliminated.** With a 200ms lookahead, both pipelines can check for "future" peaks before committing to a current one. This prevents the "double-hit" issue in Max where a later, larger peak causes a regression of a previously detected smaller peak.
3.  **Inter-band Processing Order**:
    - *Impact*: **No Change (unless implemented).** This is a host-side iteration issue. Even with 15s windows, Max would still process Band 0 then Band 1 unless the `analyze~.c` loop is changed to sort chronologically.
4.  **Resonance Context**:
    - *Impact*: **Eliminated.** Since the resonance lookback is 5s and the window is 15s, a 15s window ensures that the "context" available to the C core is identical in both environments.
5.  **Mel Spectrogram Normalization (Spectral Breathing)**:
    - *Impact*: **Harmonized.** Both environments would now experience "Spectral Breathing" in the exact same way. While breathing is generally undesirable, having it be *consistent* between offline and real-time is a significant win for predictability.
6.  **Temporal Resolution**:
    - *Impact*: **No Change.** This depends on the system sample rate, not the windowing strategy.
7.  **STFT Padding and Window Edges**:
    - *Impact*: **Eliminated.** By using a lookahead/lookbehind buffer and a consistent window size, the padding artifacts would occur at the same relative positions in both pipelines, effectively making the "usable" part of the flux envelope identical.

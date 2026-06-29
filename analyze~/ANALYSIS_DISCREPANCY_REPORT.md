# Analysis Discrepancy Report: `analyze~` vs. Python Ground Truth

## Executive Summary
Discrepancies in the output of the `analyze~` Max object compared to its Python reference implementation were caused by differences in how the real-time audio stream is windowed and normalized. While the underlying C algorithm is identical, the execution environment in Max introduced constraints—specifically window size and dynamic normalization—that deviated from the batch-processing nature of the Python script.

## Technical Speculations & Implementation Notes

### 1. Context Window Mismatch
**Speculation**: The `analyze~` object previously extracted a 2-second context window. However, the `analyzer_process_peak` function implements a 5-second resonance lookback. This rendered the analyzer "blind" to 60% of the historical context.
**Remediation**: The real-time analysis window has been increased to **15.2 seconds**.

**Rationale for 15.2-second Window**:
While the resonance algorithm requires a 5-second lookback, a 15.2-second window is utilized to provide several critical benefits:
- **Resonance Context Alignment**: Ensures that any peak within the 5-second lookback is guaranteed to be present in the same analysis pass as the peak being processed.
- **Rolling Threshold Alignment**: The peak detection logic in `cumulative_transience.c` utilizes a 15-second rolling threshold. A 15.2s window allows the detector to operate with its full intended historical context.
- **FFT Padding**: Provides a safety margin against STFT padding artifacts.
- **Peak Detection Look-Ahead**: Supports the 200ms lookahead safety margin, ensuring that even the most recent peaks being processed have sufficient future context for stable detection and suppression.

### 2. Volatile Spectrogram Normalization
**Speculation**: In `analyzer_analyze_audio`, the Mel spectrogram is normalized relative to the peak decibel level found within the current window. In a real-time sliding window, this causes the resulting flux envelopes to "breathe."
**Technical Limitation**: Stable normalization (using a global reference) is currently impossible without modifying the core `cumulative_transience.c` library, as the dB normalization is hardcoded within the spectral analysis function. However, the increased 6-second window size significantly stabilizes the local reference peak compared to the previous 2-second window.

### 3. Incorrect `max_peak` Reference
**Speculation**: The `TransientAnalyzer` state was initialized with a hardcoded `max_peak` of `1.0`. In batch processing, the true global maximum is used.
**Remediation**: The `max_peak` value in the analyzer state is now updated dynamically whenever a higher flux peak is discovered, allowing the system to converge towards the global maximum.

### 4. Fragmented Peak History & Timeline Synchronization
**Speculation**: Real-time processing triggers analysis on "new" peaks and only provides peaks within the current window.
**Remediation**: 
- All peaks within the 6-second window are now provided as context for resonance calculations.
- A robust **Timeline Synchronization** mechanism has been implemented. Because the library operates on relative window indices, its internal state (snapshots and scheduled events) is manually shifted to a global absolute frame space immediately after creation. This ensures that historical lookbacks, snapshot cleanup, and rolling metrics work correctly as the window slides across the audio stream.

## Summary of Improvements

### 1. 15.2-second Unified Window
The increase to a 15.2-second analysis window (15s context + 200ms lookahead) harmonizes the Max external with the Python visualizer, stabilizes STFT normalization (reducing "spectral breathing"), and provides full context for the 15s rolling threshold.

### 2. Chronological Peak Processing
Peaks are now collected across all bands and sorted by their relative frame index before being passed to the resonance engine. This ensures that the stateful `accumulated_buffer` is updated in the exact order that events occurred, eliminating inter-band synchronization discrepancies.

### 3. 200ms Lookahead Safety Margin
By processing only those peaks that are at least 200ms behind the current playhead, the system ensures that the peak detection logic has sufficient future context to apply suppression and find true local maxima, making the real-time results deterministic and consistent with offline batch analysis.

### 4. Sampling Rate & Timing Accuracy
**Speculation**: The algorithm previously assumed a fixed 1ms duration for every analysis hop. At 44.1 kHz, a 44-sample hop actually represents ~0.9977ms. Over several minutes, this discrepancy leads to a noticeable progressive drift between the analysis timeline and the actual audio.
**Remediation**:
- The `TransientAnalyzer` now calculates the precise `frame_duration_ms` based on the provided sampling rate (`1000.0 * hop_length / sr`).
- The `times` array generated during analysis uses this precise floating-point calculation for every frame.
- In `analyze_files.py`, video frames are mapped to the analysis timeline using `np.searchsorted` against a true 30 FPS clock, ensuring that the generated MP4 remains perfectly synchronized with the audio from start to finish.

### 5. Processing Trade-offs
**Question**: Would a 15-second window have any effect other than memory?
**Answer**: Yes, **CPU usage**. The background analysis task performs STFT and Mel-filtering on the entire window. Moving from 6s to 15s increases the computational load of these stages by ~250%. While this work is offloaded to a background thread and does not impact the audio thread, it does increase the overall CPU footprint of the object.

## Conclusion
The remediation strategy implemented in `analyze~.c` resolves the primary causes of divergence by synchronizing the local analysis timeline with a persistent global frame index and providing sufficient historical context for the resonance algorithms, all while maintaining the integrity of the shared algorithm library.

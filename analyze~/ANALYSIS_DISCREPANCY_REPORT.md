# Analysis Discrepancy Report: `analyze~` vs. Python Ground Truth

## Executive Summary
Discrepancies in the output of the `analyze~` Max object compared to its Python reference implementation were caused by differences in how the real-time audio stream is windowed and normalized. While the underlying C algorithm is identical, the execution environment in Max introduced constraints—specifically window size and dynamic normalization—that deviated from the batch-processing nature of the Python script.

## Technical Speculations & Implementation Notes

### 1. Context Window Mismatch
**Speculation**: The `analyze~` object previously extracted a 2-second context window. However, the `analyzer_process_peak` function implements a 5-second resonance lookback. This rendered the analyzer "blind" to 60% of the historical context.
**Remediation**: The real-time analysis window has been increased to **6 seconds**. Since analysis is triggered only on "new" peaks appearing at the end of the sliding window, this 6-second window provides the full 5-second historical context required by the resonance algorithms.

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

## Conclusion
The remediation strategy implemented in `analyze~.c` resolves the primary causes of divergence by synchronizing the local analysis timeline with a persistent global frame index and providing sufficient historical context for the resonance algorithms, all while maintaining the integrity of the shared algorithm library.

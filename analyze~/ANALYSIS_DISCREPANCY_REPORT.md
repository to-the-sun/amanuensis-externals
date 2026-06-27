# Analysis Discrepancy Report: `analyze~` vs. Python Ground Truth

## Executive Summary
Discrepancies in the output of the `analyze~` Max object compared to its Python reference implementation were caused by differences in how the real-time audio stream is windowed and normalized. While the underlying C algorithm is identical, the execution environment in Max introduced constraints—specifically window size and dynamic normalization—that deviated from the batch-processing nature of the Python script.

## Technical Speculations & Implementation Notes

### 1. Context Window Mismatch
**Speculation**: The `analyze~` object previously extracted a 2-second context window. However, the `analyzer_process_peak` function implements a 5-second resonance lookback. This rendered the analyzer "blind" to 60% of the historical context.
**Remediation**: The real-time analysis window has been increased to **6 seconds**. 

**Rationale for 6-second vs. 5-second Window**:
While the resonance algorithm requires a 5-second lookback, a 6-second window is utilized to provide a critical "safety margin" for the following reasons:
- **FFT Padding**: The spectral analysis phase (`analyzer_analyze_audio`) uses an STFT that artificially zeros out approximately 23ms of the envelope at the start and end of the buffer due to FFT padding. A 6-second window ensures that even when looking back 5 seconds, the algorithm is accessing "clean" spectral data unaffected by these padding artifacts.
- **Peak Detection Look-Ahead**: The peak detection logic identifies peaks by comparing a frame to its neighbors (`f-1`, `f+1`). This means detected peaks are always slightly delayed from the absolute end of the buffer. The extra second of window length ensures that the "newest" detected peaks still have a full 5 seconds of valid historical data behind them, preventing the algorithm from zero-filling the lookback snapshots.
- **Internal Algorithmic Guardrails**: Although 6 seconds of peak data are provided to the analyzer, the core `cumulative_transience.c` library internally filters this list. The `analyzer_process_peak` function strictly considers only peaks within the range `[p_idx - 5000, p_idx - 99]` relative to the peak being analyzed. Thus, the extra data provides a buffer for detection but is programmatically excluded from the final resonance calculation, ensuring exact alignment with the 5-second specification.

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

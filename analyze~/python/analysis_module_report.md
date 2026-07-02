# Technical Report: Cumulative Transience Analysis Module

This report describes the architecture and interface of the `cumulative_transience` Python module, which provides a high-performance Cython bridge to the core C analysis library.

## 1. Module Overview
The module is encapsulated in the `TransientAnalyzer` class (within `ct_extension.pyx`). It maintains the state of the spectral analysis, historical resonance buffer, and rolling metrics.

### **Internal State**
- **C-Level Struct**: Manages a pointer to the `TransientAnalyzer` C structure.
- **Buffers**: Includes the Mel spectrogram cache, flux envelopes, and the 5-second `accumulated_buffer`.
- **Deduplication**: Uses `_processed_peaks` to track processed transients within the Python environment.

#### **Core Methods**

##### `process_new_peaks(frame, ...)`
Processes detected peaks that fall within a 100ms window preceding the current `frame`.
- **Logic**: Calls `analyzer_process_peak` (C) for each new peak, implementing the 99ms resonance exclusion zone.
- **Outputs**: Returns detailed data for each peak, including the resonance score, individual qualifiers, and the 5001-sample snapshot.

##### `update_metrics(frame)`
Performs state cleanup and calculates current metrics.
- **Logic**: Removes snapshots older than 15 seconds from the accumulated buffer and updates statistical metrics (Std Dev, Contrast, Rating).

---

### 2. Batch Analysis Function

#### `analyze_audio(y, sr)`
Performs full spectral decomposition and feature extraction on a raw audio array.
- **Implementation**: Calls `analyzer_batch_analyze` (C), which internally simulates the incremental process in 100ms chunks to ensure bit-perfect parity with real-time analysis.
- **Details**: Performs STFT (2048 window, 1ms hop), applies a 128-bin Mel-filterbank, calculates flux across 4 bands, and identifies peaks using the adaptive midpoint model.

---

## Technical Specifications

| Parameter | Value | Description |
| :--- | :--- | :--- |
| **Temporal Resolution** | ~1ms | All internal buffers and indices operate at 1ms steps. |
| **Resonance Buffer** | 5001 samples | 5-second lookback window. |
| **Spectral Bands** | 4 | Mel bands split into 4 equal bands of 32 bins each. |
| **Exclusion Zone** | 99ms | Resonance lookback ignores the most recent 99ms. |
| **State Retention** | 15s | Snapshots persist in the accumulated buffer for 15 seconds. |
| **Peak Suppression** | 200ms | Minimum distance enforced between peaks in the same band. |
| **Thresholding** | Midpoint | 999ms rolling midpoint used for detection and prominence. |

---

## Architecture: Cython vs. C
1.  **`cumulative_transience.c / .h`**: Contains the raw DSP algorithms (FFT, Mel-Filtering, Flux, Peak Detection, Resonance).
2.  **`ct_extension.pyx`**: The Cython wrapper handling NumPy memory views, Python dictionary creation, and the high-level class interface.

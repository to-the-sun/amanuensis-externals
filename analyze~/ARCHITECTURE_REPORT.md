# Architectural Boundary Report: Transient Analysis System

This report outlines the boundaries between the core C analysis algorithm and the various consumers (Max MSP and Python) that utilize it.

---

## 1. The Core Engine: `cumulative_transience.c / .h`
The "Source of Truth" for all numerical analysis. It is platform-agnostic and contains no dependencies on Max SDK or Python headers.

### **Responsibilities**
*   **Signal Processing**: Implementation of a Radix-2 FFT and Mel-Filterbank.
*   **Feature Extraction**: Calculation of Spectral Flux (onset strength) and a `dynamic_smoothing` parameter across 4 bands with 80dB noise floor clamping.
*   **Peak Detection**: Algorithmic identification of transients based on dynamic historical peak-based rolling midpoints and adaptive prominence checks.
*   **Resonance Scoring**: Maintaining a 5-second historical buffer (`accumulated_buffer`) and calculating "qualifier" scores using a 99ms exclusion zone.
*   **State Management**: The `TransientAnalyzer` struct tracks rolling metrics and handles the 15-second state cleanup.

### **Boundary Interface**
*   **Inputs**: Raw `float*` audio buffers, sample rates, and frame indices.
*   **Outputs**: C structs containing raw numerical data.
    *   **`PeakResult`**: Provides individual resonance scores and qualifiers in real-time.
    *   **`AnalyzerMetrics`**: Provides persistent metrics (Std Dev, Contrast, Rating, etc.).
*   **Memory**: The host is responsible for audio buffer allocation; the library manages its internal analyzer state.

---

## 2. The Max External: `analyze~.c`
The real-time bridge for the Cycling '74 Max environment.

### **Responsibilities**
*   **Audio Ingestion**: Captures live signals in the high-priority DSP thread.
*   **Threading**: Offloads analysis to a background `async_worker` to prevent audio dropouts.
*   **Timeline Synchronization**: Maps local analysis indices back to global song frames to ensure consistency.
*   **Dynamic Normalization**: Updates the `max_peak` reference in real-time as higher flux values are encountered.
*   **Max Integration**: Manages inlets, outlets, and UI-thread deferral for metrics and peak messages.

---

## 3. The Python System: `analyze~/python/`
The Python implementation is built on a high-performance native extension.

### **Layer A: The Glue (`ct_extension.pyx`)**
A Cython-based native extension that compiles into a Python module.
*   **Boundary Function**: Maps NumPy arrays directly to C pointers for zero-overhead data passing.
*   **Data Conversion**: Translates fixed-size C structs into Python dictionaries.
*   **Lifecycle**: Wraps `analyzer_create` and `analyzer_destroy` within a Python class.

### **Layer B: The Orchestrator (`analyze_files.py`)**
Handles file management and high-fidelity visualization.
*   **File I/O**: Uses `librosa` for audio loading and resampling to 44.1kHz.
*   **Batch Analysis**: Utilizes the C-core batch analysis mode, which internally simulates the incremental process.
*   **Visualization**: Employs an optimized Matplotlib backend with blitting and artist pooling for efficient 30 FPS rendering.
*   **Video Rendering**: Orchestrates FFmpeg to generate synchronized video reports.

---

## 4. Normalization and `max_peak` Management

The `max_peak` value is a critical normalization constant representing the maximum spectral flux value encountered at a detected peak. It scales the 5-second snapshots before they are accumulated.

### **Determination Strategies**

| Environment | Strategy | Implementation |
| :--- | :--- | :--- |
| **Max (`analyze~`)** | **Dynamic Update** | Initializes `max_peak` at 1.0. During each 100ms analysis cycle, it updates the internal analyzer state if a higher flux value is detected. |
| **Python (`analyze_files`)** | **Dynamic Update** | Initializes at 1.0. During batch analysis, the C core dynamically updates its internal `max_peak` state as it processes the file, matching the real-time behavior. |

---

## Summary of Boundaries

| Feature | Core C (`cumulative_transience`) | Max External (`analyze~`) | Python (`analyze_files`) |
| :--- | :---: | :---: | :---: |
| **FFT / Mel-Filters** | **Primary** | - | - |
| **Peak Detection Logic** | **Primary** | - | - |
| **Resonance Scoring** | **Primary** | - | - |
| **Audio File Loading** | - | - | **librosa** |
| **Live Audio Capture** | - | **DSP Thread** | - |
| **Threading/Async** | - | **Async Worker** | **threading** |
| **Timeline Mapping** | - | **Global Frame Fixup** | **Array Indexing** |
| **Visualization/UI** | - | **Outlets/Assist** | **Matplotlib/FFmpeg** |
| **Normalization State** | **Structure Member** | **Dynamic Update** | **Dynamic Update** |

---

## Conclusion
The system is built on a **Unified C-Core**. The host environments (Max and Python) serve as data providers and result consumers, while all critical DSP and transience logic is centralized in the C library to ensure bit-perfect parity across all use cases.

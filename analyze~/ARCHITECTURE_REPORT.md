# Architectural Boundary Report: Transient Analysis System

This report outlines the boundaries between the core C analysis algorithm and the various consumers (Max MSP and Python) that utilize it.

---

## 1. The Core Engine: `cumulative_transience.c / .h`
The "Source of Truth" for all numerical analysis. It is platform-agnostic and contains no dependencies on Max SDK or Python headers.

### **Responsibilities**
*   **Signal Processing**: Implementation of a Radix-2 FFT and Mel-Filterbank.
*   **Feature Extraction**: Calculation of Spectral Flux (onset strength) across 4 bands.
*   **Peak Detection**: Algorithmic identification of transients based on local prominence and adaptive midpoints.
*   **Resonance Scoring**: Maintaining a 5-second historical buffer (`accumulated_buffer`) and calculating "qualifier" scores.
*   **State Management**: The `TransientAnalyzer` struct tracks metrics (Std Dev, Contrast, Rating) and handles the 15-second state cleanup.

### **Boundary Interface**
*   **Inputs**: Raw `float*` audio buffers, sample rates, and frame indices.
*   **Outputs**: C structs (`PeakResult`, `AnalyzerMetrics`) containing raw numerical data.
*   **Memory**: The library handles its internal analyzer state; the caller is responsible for providing audio buffers and managing the analyzer's lifecycle.

---

## 2. The Max External: `analyze~.c`
The real-time bridge for the Cycling '74 Max environment.

### **Responsibilities**
*   **Audio Ingestion**: Capturing live signals in the high-priority DSP thread.
*   **Threading**: Offloading heavy analysis to a background `async_worker`.
*   **Timeline Synchronization**: Mapping "local" analyzer indices to "global" song frames for consistent resonance history.
*   **Dynamic Normalization**: Updating the `max_peak` reference in real-time as the song progresses.
*   **Max Ecosystem Integration**: Managing inlets, outlets, and UI messaging.

---

## 3. The Python System: `analyze~/python/`
The Python implementation utilizes a layered abstraction.

### **Layer A: The Glue (`ct_extension.pyx`)**
A Cython-based native extension that compiles into a Python dynamic module.
*   **Direct Mapping**: Maps NumPy arrays directly to C pointers for zero-overhead data passing.
*   **Data Conversion**: Translates C structs into flexible Python dictionaries.

### **Layer B: The Orchestrator (`analyze_files.py`)**
Handles file management and high-level visualization.
*   **File I/O**: Uses `librosa` for audio loading and resampling to 44.1kHz.
*   **Batch Analysis**: Calls the C-core's `analyzer_batch_analyze` to process files.
*   **Dynamic UI**: Orchestrates Matplotlib and FFmpeg to generate synchronized video reports with rolling average scores.

### **Layer C: The Automation (`Amanuensis.py`)**
A Discord bot serving as an automated interface for processing files and distributing reports.

---

## 4. Normalization and `max_peak` Management

The `max_peak` value is a critical normalization constant representing the loudest spectral flux encountered. It scales the 5-second snapshots before they are accumulated, ensuring consistent resonance scores.

### **Implementation Strategies**

| Environment | Strategy | Implementation |
| :--- | :--- | :--- |
| **Max (`analyze~`)** | **Dynamic Update** | The object initializes `max_peak` at 1.0 and updates it in real-time within the background analysis cycle as higher peaks are encountered. |
| **Python (`analyze_files`)** | **Dynamic Update** | The `TransientAnalyzer` dynamically updates its internal `max_peak` state during processing, ensuring consistency with the real-time model. |

---

## Summary of Boundaries

| Feature | Core C (`cumulative_transience`) | Max External (`analyze~`) | Python (`analyze_files`) |
| :--- | :---: | :---: | :---: |
| **FFT / Mel-Filters** | **Primary** | - | - |
| **Peak Detection Logic** | **Primary** | - | - |
| **Resonance Scoring** | **Primary** | - | - |
| **Normalization State** | **Structure Member** | **Dynamic Update** | **Dynamic Update** |
| **Audio Capture/Loading** | - | **Live DSP** | **librosa** |
| **Visualization/UI** | - | **Max Outlets** | **Matplotlib/FFmpeg** |

---

## Key Interaction Points
1.  **Normalization**: The C core manages the `max_peak` state, which consumers can update or query to ensure consistent scaling.
2.  **Relative vs. Absolute**: Both Max and Python map the C-core's relative buffer indices to their respective global timelines.
3.  **Static Integration**: The system is built for bit-perfect parity; the Max external statically links the C source, while Python compiles it into a native extension.

# Architectural Boundary Report: Transient Analysis System

This report outlines the boundaries between the core C analysis algorithm and the various consumers (Max MSP and Python) that utilize it.

---

## 1. The Core Engine: `cumulative_transience.c / .h`
The "Source of Truth" for all numerical analysis. It is designed to be platform-agnostic and contains no dependencies on Max SDK or Python headers.

### **Responsibilities**
*   **Signal Processing**: Implementation of a Radix-2 FFT and Mel-Filterbank.
*   **Feature Extraction**: Calculation of Spectral Flux (onset strength) across 4 bands.
*   **Peak Detection**: Algorithmic identification of transients based on local prominence and rolling thresholds.
*   **Resonance Scoring**: Maintaining a 5-second historical buffer (`accumulated_buffer`) and calculating "qualifier" scores based on rhythm density.
*   **Rolling Score Calculation**: Maintaining a high-resolution rolling average of resonance scores (calculated over a 10ms window) for real-time visualization.
*   **State Management**: The `TransientAnalyzer` struct tracks the rolling metrics (Std Dev, Contrast, Rating, Rolling Score) and handles the "Cleanup Sweep" (removing old snapshots).

### **Boundary Interface**
*   **Inputs**: Raw `float*` audio buffers or `float*` envelope buffers, sample rates, and frame indices.
*   **Outputs**: C structs containing raw numerical data.
    *   **`PeakResult`**: Provides individual resonance scores (`total_score`) in real-time as peaks are identified.
    *   **`AnalyzerMetrics`**: Provides the 10ms rolling average (`rolling_score`) as a persistent metric.
*   **Memory**: Caller is responsible for allocating/freeing the audio buffers; the library handles its internal analyzer state.

---

## 2. The Max External: `analyze~.c`
The real-time bridge for the Cycling '74 Max environment.

### **Responsibilities**
*   **Audio Ingestion**: Capturing live signals in the high-priority DSP thread and storing them in a 60-second circular buffer.
*   **Threading**: Offloading heavy analysis to a background `async_worker` to prevent UI/Audio dropouts.
*   **Timeline Synchronization**:
    *   The C core treats every window as starting at index 0.
    *   `analyze~.c` is responsible for mapping these "local" indices back to "global" song frames so that historical resonance data remains consistent as the window slides.
*   **Dynamic Normalization**: Updating the `max_peak` reference in real-time as the song progresses.
*   **Max Ecosystem Integration**: Managing inlets, outlets, assist messages, and `defer`ing background results to the main thread for output.

---

## 3. The Python System: `analyze~/python/`
The Python implementation is split into three distinct layers of abstraction.

### **Layer A: The Glue (`ct_extension.pyx`)**
A Cython-based native extension that compiles into `cumulative_transience.pyd` (Windows) or `.so` (Linux).
*   **Boundary Function**: Maps Python NumPy arrays directly to C pointers (`<float*>env.data`).
*   **Data Conversion**: Translates fixed-size C structs (like `PeakResult`) into flexible Python dictionaries.
*   **Lifecycle**: Manages the `analyzer_create` and `analyzer_destroy` calls within a Python class wrapper.

### **Layer B: The Orchestrator (`analyze_files.py`)**
Handles the "heavy lifting" of file management and visualization.
*   **File I/O**: Uses `librosa` to load and resample various audio formats (MP3, WAV, FLAC).
*   **Batch Analysis**: Calls `analyze_audio()` to process entire files at once.
*   **Graphing**: Uses `Matplotlib` to render the 4-band transient envelopes and rolling thresholds.
*   **Video Rendering**: Orchestrates `FFmpeg` and `Matplotlib.animation` to generate the synchronized video reports.
*   **Logic**: Handles the visual "flash and fade" effects and score animations seen in the videos.

### **Layer C: The Automation (`Amanuensis.py`)**
A Discord bot that serves as a high-level UI.
*   **Integration**: Monitors file system changes and triggers `analyze_files.py` in a background thread.
*   **Distribution**: Uploads generated videos and metrics to Discord.

---

## Summary of Boundaries

| Feature | Core C (`cumulative_transience`) | Max External (`analyze~`) | Python (`analyze_files`) |
| :--- | :---: | :---: | :---: |
| **FFT / Mel-Filters** | **Primary** | - | - |
| **Peak Detection Logic** | **Primary** | - | - |
| **Resonance Scoring** | **Primary** | - | - |
| **Rolling Score (10ms Avg)** | **Primary** | - | - |
| **Audio File Loading** | - | - | **librosa** |
| **Live Audio Capture** | - | **DSP Thread** | - |
| **Threading/Async** | - | **Async Worker** | **asyncio/threads** |
| **Timeline Mapping** | - | **Global Frame Fixup** | **Array Indexing** |
| **Visualization/UI** | - | **Outlets/Assist** | **Matplotlib/FFmpeg** |
| **Normalization State** | **Structure Member** | **Dynamic Update** | **Batch Global Max** |

---

## Key Interaction Points
1.  **Normalization**: The C code performs dB normalization *per window*. The Max object and Python script must both manage the `max_peak` value to ensure the relative flux values are scaled correctly across time.
2.  **Relative vs. Absolute**: The C code is "dumb" regarding time—it only knows about the indices in the buffer it was given. Both Max and Python have custom logic to "offset" these indices to match the actual timeline of the audio file or performance.
3.  **Static Linking**: The Max external statically links `cumulative_transience.c`. The Python extension compiles it into a module. This ensures they both run identical algorithmic logic while providing completely different user experiences.

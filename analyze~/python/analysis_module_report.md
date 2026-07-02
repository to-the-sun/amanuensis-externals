# Transient Analysis Module Report: `cumulative_transience` (Cython Extension)

## Overview
The `cumulative_transience` module serves as the high-performance core engine for audio transient analysis. It is implemented as a native Cython-based Python extension, wrapping optimized C code (`cumulative_transience.c`). This architecture separates heavy numerical analysis from high-level orchestration, ensuring that calculations are performant enough for real-time applications.

---

## Core Components

### 1. `TransientAnalyzer` Class (Cython Wrapper)
The stateful object for managing cumulative transient analysis, providing a Pythonic interface to the underlying C structure.

#### **State Management**
- **C-Level State (`TransientAnalyzer` struct)**:
    - `accumulated_buffer`: A 5001-sample (5 seconds @ 1ms resolution) buffer representing the sum of historical transient snapshots.
    - `buffer_times`: Mapping of buffer indices to relative time offsets (-5000ms to 0ms).
    - `max_peak`: Global normalization factor for input spectral flux.
    - `min_score_seen` / `max_score_seen`: Dynamic tracking of the resonance score range.
    - `total_score_sum` / `score_count`: Used for calculating the overall rating (average resonance).
    - `peak_history`: A historical record of detected peak timestamps (up to `MAX_PEAK_HISTORY=8192`).
    - `snapshot_heads` / `snapshot_tails`: Per-band queues of `SnapshotEntry` objects used for 15-second state cleanup.
- **Cython-Level State**:
    - `_processed_peaks`: A list of four sets (one per band) used to deduplicate peak processing within the Python environment.

#### **Methods**

##### `process_new_peaks(frame, peak_indices_list, onset_envs, all_valid_peak_indices, times)`
Processes detected peaks that fall within a 100ms window preceding the current `frame`.
- **Logic**:
    - Identifies peaks in `peak_indices_list` that are between `frame - 100` and `frame` and have not been processed yet.
    - Calls `analyzer_process_peak` (C) for each new peak.
    - Implements a **99ms resonance exclusion zone**: resonance is calculated against the accumulated buffer excluding the most recent 99ms to avoid self-correlation.
- **Inputs**:
    - `frame` (int): Current playhead position in milliseconds.
    - `peak_indices_list` (list of lists): Pre-detected peak indices for each of the 4 spectral bands.
    - `onset_envs` (list of arrays): Raw onset strength envelopes for each band.
    - `all_valid_peak_indices` (set/list): Union of all peak indices across bands.
    - `times` (array): Time mapping for frames.
- **Outputs**: A list of dictionaries containing:
    - `p_idx` (int): Frame index of the peak.
    - `band_idx` (int): Spectral band index (0-3).
    - `time` (float): Absolute timestamp in seconds.
    - `peak_val` (float): Normalized flux value at the peak.
    - `total_score` (float): Calculated resonance score.
    - `qualifiers` (list of dicts): Individual resonance contributors with `ms` (offset) and `val` (weight).
    - `snapshot` (numpy.ndarray): The 5001-sample snapshot added to the buffer.

##### `update_metrics(frame)`
Performs buffer cleanup and calculates rhythm and scoring metrics.
- **Logic**:
    - Removes snapshots older than 15 seconds (`frame - 15000`) from the accumulated buffer.
    - Calculates statistical metrics (std_dev, mean, contrast) from the current buffer state.
- **Inputs**: `frame` (int): Current playhead position.
- **Outputs**: A dictionary containing:
    - `std_dev` (float): Standard deviation of the accumulated buffer.
    - `mean` (float): Mean value of the accumulated buffer.
    - `contrast` (float): Peak-to-mean ratio.
    - `peak_std` (float): Standard deviation of the peak history timestamps.
    - `rating` (float): Running average of all `total_score` values.
    - `buffer_updated` (bool): True if snapshots were removed during this call.
    - `highest_peak_ms` (float or None): Relative time (ms) of the largest resonance peak in the buffer.
    - `min_score_seen` (float): Lowest resonance score recorded.
    - `max_score_seen` (float): Highest resonance score recorded.

---

### 2. Standalone Functions

#### `analyze_audio(y, sr)`
Performs full spectral decomposition and feature extraction on raw audio.
- **Logic**:
    - Performs STFT with 2048-sample window and 1ms hop size.
    - Applies a 128-bin Mel-filterbank.
    - **Spectral Decomposition**: Splits the 128 Mel bins into **4 bands** (32 bins each: 0-31, 32-63, 64-95, 96-127).
    - Calculates spectral flux (positive difference) for each band.
    - Detects peaks using a 15-second rolling threshold and prominence/distance constraints.
- **Inputs**:
    - `y` (numpy.ndarray): Mono audio data (float32).
    - `sr` (int): Sample rate.
- **Outputs**: A dictionary containing:
    - `times` (numpy.ndarray): Timestamp array (1ms resolution).
    - `max_peak_value` (float): Highest flux value found across all bands/peaks.
    - `onset_envs` (list of arrays): Raw spectral flux envelopes for the 4 bands.
    - `rolling_thresholds` (list of arrays): 15-second rolling mean thresholds for each band.
    - `peaks_list` (list of arrays): Detected peak indices for each band.
    - `ratings` (numpy.ndarray): Historical resonance ratings (if called via `batch_analyze`).

---

## Technical Specifications

| Parameter | Value | Description |
| :--- | :--- | :--- |
| **Temporal Resolution** | 1ms | All internal buffers and indices operate at 1ms steps. |
| **Buffer Length** | 5001 | 5-second lookback + 1 current sample. |
| **Spectral Bands** | 4 | Mel bands split: [0-31], [32-63], [64-95], [96-127]. |
| **Exclusion Zone** | 99ms | Resonance lookback ignores the most recent 99ms. |
| **State Retention** | 15s | Snapshots persist in the accumulated buffer for 15 seconds. |
| **Peak Detection Distance** | 200ms | Minimum distance enforced between peaks in the same band. |
| **Peak Prominence** | > Rolling Median | Minimum prominence required for flux peak detection. |
| **Noise Floor** | 0.0 | Absolute flux floor (dB) for transient identification. |

---

## Architecture: Cython vs. C
The module is built from three primary components:
1.  **`cumulative_transience.h`**: Defines the shared data structures (`PeakResult`, `AnalyzerMetrics`, `TransientAnalyzer`) and function signatures used by both Python and Max MSP.
2.  **`cumulative_transience.c`**: Contains the raw numerical DSP algorithms (FFT, Mel-Filterbank, Spectral Flux, Peak Detection, Resonance Scoring).
3.  **`ct_extension.pyx`**: A Cython wrapper that handles NumPy array memory management, Python dictionary creation, and high-level deduplication logic (`_processed_peaks`).

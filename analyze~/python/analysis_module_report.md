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
    - `max_peak`: Global normalization factor for input spectral flux.
    - `min_score_seen` / `max_score_seen`: Dynamic tracking of the resonance score range.
    - `peak_history`: A historical record of detected peak timestamps (up to `MAX_PEAK_HISTORY=8192`).
    - `snapshot_heads` / `snapshot_tails`: Per-band queues of `SnapshotEntry` objects used for 15-second state cleanup.
- **Cython-Level State**:
    - `_processed_peaks`: A list of four sets (one per band) used to deduplicate peak processing within the Python environment.

#### **Methods**

##### `analyze_chunk(y, sr, buffer_start_frame, active_start_frame)`
The primary entry point for real-time-style chunked analysis.
- **Logic**:
    - Hands a 100ms audio chunk to the C core.
    - Performs FFT, Mel-filtering, and flux calculation with stateful overlap.
    - Extracts peaks and updates resonance metrics within the specified context frames.
- **Outputs**: A dictionary containing `peaks`, `flux` envelopes, and `metrics`.

##### `update_metrics(frame)`
Performs buffer cleanup and calculates rhythm and scoring metrics.
- **Logic**:
    - Removes snapshots older than 15 seconds from the accumulated buffer.
    - Calculates statistical metrics (std_dev, mean, contrast) and identifies the highest historical peak.
- **Outputs**: A dictionary containing current buffer metrics and the `highest_peak_ms`.

---

### 2. Standalone Functions

#### `analyze_audio(y, sr)`
Performs full batch analysis on raw audio by internally simulating the chunked process.
- **Logic**:
    - Orchestrates the C core to process the entire audio file in 100ms steps.
    - Ensures that the resulting envelopes and peaks match the real-time behavior.
- **Outputs**: A dictionary containing `times`, `onset_envs`, `peaks`, and historical metrics for the entire file.

---

## Technical Specifications

| Parameter | Value | Description |
| :--- | :--- | :--- |
| **Temporal Resolution** | 1ms | All internal buffers and indices operate at 1ms steps. |
| **Buffer Length** | 5001 | 5-second lookback + 1 current sample. |
| **Spectral Bands** | 4 | Mel bands split: [0-31], [32-63], [64-95], [96-127]. |
| **Exclusion Zone** | 99ms | Resonance lookback ignores the most recent 99ms. |
| **State Retention** | 15s | Snapshots persist in the accumulated buffer for 15 seconds. |
| **Peak Distance** | 200ms | Minimum distance enforced between peaks in the same band. |
| **Peak Prominence** | > Midpoint | Minimum prominence required for flux peak detection. |
| **Noise Floor** | 0.0 | Absolute flux floor (dB) for transient identification. |

---

## Architecture: Cython vs. C
The module is built from three primary components:
1.  **`cumulative_transience.h`**: Defines the shared data structures used by both Python and Max MSP.
2.  **`cumulative_transience.c`**: Contains the raw numerical DSP algorithms.
3.  **`ct_extension.pyx`**: A Cython wrapper that handles NumPy array memory management and provides the Pythonic interface.

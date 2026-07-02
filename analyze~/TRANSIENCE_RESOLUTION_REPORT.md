# Transience Analysis Resolution Report

This report documents the hierarchical levels of resolution within the `cumulative_transience` system, ranging from raw audio samples to global state management.

---

## 1. Sample Rate (Raw Signal)
*   **Resolution**: ~0.02ms (at 44.1 kHz)
*   **Source**: `analyze~.c` (DSP Thread), `analyze_files.py` (`librosa.load`)
*   **Purpose**: The system ingests raw audio buffers at the sample rate. All DSP operations (FFT, Mel-Filtering) begin at this level.

## 2. Analysis Hop Size (Fundamental Frame)
*   **Resolution**: 1.0ms (at 48 kHz) / 0.9977ms (at 44.1 kHz)
*   **Source**: `cumulative_transience.c` (`hop_length = (int)(sr * 0.001)`)
*   **Purpose**: This is the "Base Unit" of the algorithm. All spectral flux calculations, envelope generation, and peak indices are quantized to this ~1ms resolution.

## 3. Video Animation Step (Visual Frame)
*   **Resolution**: 33.33ms (30 FPS)
*   **Source**: `analyze_files.py` (`fps=30`)
*   **Purpose**: The interval at which the Python orchestrator renders a new frame. Audio analysis frames are mapped to this 30 FPS video clock.

## 4. Visual Rolling Score Window
*   **Resolution**: 39ms
*   **Source**: `analyze_files.py` (`rolling_window_scores`)
*   **Purpose**: A sliding window used in the Python visualizer. The displayed "Score" is the average of all scores within the most recent 39ms.

## 5. Resonance Exclusion Zone
*   **Resolution**: 99ms
*   **Source**: `cumulative_transience.c` (Excluding last 99ms)
*   **Purpose**: When a peak is processed, the algorithm ignores the most recent 99ms of history to prevent "self-resonance."

## 6. Processing Hop (Background Trigger)
*   **Resolution**: 100ms
*   **Source**: `analyze~.c` (`ANALYSIS_HOP_MS 100`)
*   **Purpose**: The `analyze~` Max object triggers its background analysis task at this interval for near real-time performance.

## 7. Minimum Peak Distance
*   **Resolution**: 200ms
*   **Source**: `cumulative_transience.c` (`f - last_peak < 200`)
*   **Purpose**: The minimum separation required between two peaks within the same frequency band.

## 8. Resonance Lookback Window
*   **Resolution**: 5,000ms (5 Seconds)
*   **Source**: `cumulative_transience.c` (`BUFFER_LEN 5001`)
*   **Purpose**: The primary window of historical context for calculating resonance scores.

## 9. Analysis Context Cache
*   **Resolution**: 15,200ms (15.2 Seconds)
*   **Source**: `cumulative_transience.c` (`CACHE_SIZE 15201`)
*   **Purpose**: The internal circular buffer size for the Mel spectrogram and flux envelopes, maintaining history for thresholding and lookahead.

## 10. Midpoint Sub-Window
*   **Resolution**: Dynamic (100ms - 15000ms)
*   **Source**: `cumulative_transience.c` (Based on per-band peak density)
*   **Purpose**: The sub-window at the end of the cache used to calculate the rolling midpoint for thresholding. Calculated as `15000ms - avg_peak_delta`, where `avg_peak_delta` is the average time between peaks found within the *current* lookback window for that frequency band.

## 11. State Cleanup Threshold
*   **Resolution**: 15,000ms (15 Seconds)
*   **Source**: `cumulative_transience.c` (`cleanup = frame - 15000`)
*   **Purpose**: The threshold at which snapshots are removed from the accumulated resonance buffer.

## 12. Global Audio Buffer
*   **Resolution**: 60,000ms (60 Seconds)
*   **Source**: `analyze~.c` (`MAX_AUDIO_SECONDS 60`)
*   **Purpose**: The maximum amount of recent audio history maintained in RAM by the `analyze~` object.

---

### Summary Table

| Level | Resolution (ms) | Category | Domain |
| :--- | :--- | :--- | :--- |
| **Sample Rate** | ~0.02 | Signal | DSP |
| **Analysis Hop** | 1.0 | Base Unit | C Core |
| **Video Step** | 33.3 | UI | Python |
| **Rolling Window**| 39.0 | UI | Python |
| **Exclusion Zone**| 99.0 | Logic | C Core |
| **Processing Hop**| 100.0 | Orchestration | Max MSP |
| **Peak Distance** | 200.0 | Logic | C Core |
| **Lookback Window**| 5,000.0 | Context | C Core |
| **Context Cache** | 15,200.0| Integration | C Core |
| **Midpoint Window** | dynamic | Logic | C Core |
| **State Cleanup** | 15,000.0 | Management | C Core |
| **Global Buffer** | 60,000.0 | Storage | Max MSP |

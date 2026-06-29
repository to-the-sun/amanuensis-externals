# Transience Analysis Resolution Report

This report documents the hierarchical levels of resolution within the `cumulative_transience` system, ranging from raw audio samples to global state management.

---

## 1. Sample Rate (Raw Signal)
*   **Resolution**: ~0.02ms (at 44.1 kHz) / ~0.021ms (at 48 kHz)
*   **Source**: `analyze~.c` (DSP Thread), `analyze_files.py` (`librosa.load`)
*   **Purpose**: The highest resolution available. The system ingests raw audio buffers at the hardware or file sample rate. All DSP operations (FFT, Mel-Filtering) begin at this level.

## 2. Analysis Hop Size (Fundamental Frame)
*   **Resolution**: 1.0ms (at 48 kHz) / 0.9977ms (at 44.1 kHz)
*   **Source**: `cumulative_transience.c` (`hop_length = (int)(sr * 0.001)`)
*   **Purpose**: This is the "Base Unit" of the algorithm. All spectral flux calculations, envelope generation, and peak indices are quantized to this ~1ms resolution. The system now uses precise timing (`hop_length / sr`) to prevent cumulative synchronization drift.

## 3. Video Animation Step (Visual Frame)
*   **Resolution**: 33.333...ms (30 FPS)
*   **Source**: `analyze_files.py` (`fps=30`, `np.searchsorted`)
*   **Purpose**: The interval at which the Python orchestrator renders a new frame for the MP4 report. Audio analysis frames are precisely mapped to the 30 FPS video clock to ensure long-term synchronization.

## 4. Visual Rolling Score Window
*   **Resolution**: 39ms (Window Size)
*   **Source**: `analyze_files.py` (`rolling_window_scores = [s for s in rolling_window_scores if s['frame'] > frame - 39]`)
*   **Purpose**: A sliding window used in the Python "39ms Rolling Window Snapshot" visualization. At each 30 FPS video frame, the system prunes the current score pool to the most recent 39ms. It calculates the average of these scores and displays them on the snapshot bar, aligned relative to the latest peak in the window.

## 5. Resonance Exclusion Zone
*   **Resolution**: 99ms
*   **Source**: `cumulative_transience.c` (`p_idx - 99`, `BUFFER_LEN - 99`)
*   **Purpose**: When a peak is processed, the algorithm ignores the most recent 99ms of history. This prevents "self-resonance" and ensures that the score is based on distinct rhythmic relationships rather than the tail of the current transient.

## 6. Processing Hop (Background Trigger)
*   **Resolution**: 100ms
*   **Source**: `analyze~.c` (`ANALYSIS_HOP_MS 100`)
*   **Purpose**: The `analyze~` Max object triggers its background analysis task at this interval. This ensures that analysis is "near real-time" without overwhelming the CPU by attempting to re-analyze on every single audio vector.

## 7. Minimum Peak Distance
*   **Resolution**: 200ms
*   **Source**: `cumulative_transience.c` (`f - temp_peaks[peak_count-1] < 200`)
*   **Purpose**: The minimum separation required between two peaks within the same frequency band. If a new peak is detected within 200ms of the previous one, the algorithm only keeps the one with the higher magnitude.

## 8. Resonance Lookback Window
*   **Resolution**: 5,000ms (5 Seconds)
*   **Source**: `cumulative_transience.c` (`BUFFER_LEN 5001`, `start = p_idx - 5000`)
*   **Purpose**: The primary window of historical context. For every new transient, the algorithm looks back exactly 5 seconds into the `accumulated_buffer` to calculate its cumulative resonance score based on previous rhythmic activity.

## 9. Analysis Context Window
*   **Resolution**: 6,000ms (6 Seconds)
*   **Source**: `analyze~.c` (`analysis_seconds = 6`)
*   **Purpose**: The amount of audio linearized from the circular buffer and sent to the C core in the Max environment. The extra 1 second (beyond the 5s lookback) provides a safety margin for FFT padding artifacts and peak detection latency, ensuring that any peak found has a full 5s of valid history available.

## 10. Rolling Threshold & State Cleanup
*   **Resolution**: 15,000ms (15 Seconds)
*   **Source**: `cumulative_transience.c` (`window_size = 15000`, `cleanup_frame_threshold = frame - 15000`)
*   **Purpose**:
    *   **Thresholding**: The window size for the adaptive rolling threshold used to identify peaks.
    *   **Memory Management**: The "Cleanup Sweep" threshold. The `TransientAnalyzer` removes snapshot data for transients older than 15 seconds to maintain a stable memory footprint.

## 11. Global Circular Buffer
*   **Resolution**: 60,000ms (60 Seconds)
*   **Source**: `analyze~.c` (`MAX_AUDIO_SECONDS 60`)
*   **Purpose**: The maximum amount of recent audio history maintained in RAM by the `analyze~` object. This allows the 6-second context window to be extracted from any point in the recent past as the song progresses.

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
| **Context Window** | 6,000.0 | Integration | Max MSP |
| **Cleanup/Thresh** | 15,000.0 | Management | C Core |
| **Global Buffer** | 60,000.0 | Storage | Max MSP |

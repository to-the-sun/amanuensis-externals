# Transience Analysis Resolution Report

This report documents the hierarchical levels of resolution within the `cumulative_transience` system, ranging from raw audio samples to global state management.

---

## 1. Sample Rate (Raw Signal)
*   **Resolution**: ~0.02ms (at 44.1 kHz) / ~0.021ms (at 48 kHz)
*   **Implementation**: Raw audio buffers are ingested at the hardware or file sample rate. All DSP operations begin at this level.

## 2. Analysis Hop Size (Fundamental Frame)
*   **Resolution**: 1.0ms (at 48 kHz) / 0.9977ms (at 44.1 kHz)
*   **Implementation**: This is the "Base Unit" of the algorithm. All spectral flux, envelopes, and peak indices are quantized to this ~1ms resolution. The system uses precise timing (`hop_length / sr`) to prevent synchronization drift.

## 3. Video Animation Step (Visual Frame)
*   **Resolution**: 33.33ms (30 FPS)
*   **Implementation**: The interval at which the Python orchestrator (`analyze_files.py`) renders a frame. Audio analysis frames are mapped to this clock to ensure long-term synchronization in video reports.

## 4. Visual Rolling Score Window
*   **Resolution**: 39ms
*   **Implementation**: A sliding window used in the "39ms Rolling Window Snapshot" visualization. It calculates the average of resonance scores for all peaks within the most recent 39ms relative to the playhead.

## 5. Resonance Exclusion Zone
*   **Resolution**: 99ms
*   **Implementation**: When a peak is processed, the algorithm ignores the most recent 99ms of history in the `accumulated_buffer` to prevent "self-resonance" from biasing the score.

## 6. Processing Hop (Background Trigger)
*   **Resolution**: 100ms
*   **Implementation**: The `analyze~` Max object triggers its background analysis task at this interval to maintain near real-time performance without overwhelming the CPU.

## 7. Minimum Peak Distance
*   **Resolution**: 200ms
*   **Implementation**: The minimum separation required between two peaks within the same frequency band. If a new peak is detected within 200ms of a previous one, the algorithm only keeps the one with the higher magnitude.

## 8. Resonance Lookback Window
*   **Resolution**: 5,000ms (5 Seconds)
*   **Implementation**: The primary window of historical context. For every new transient, the algorithm looks back exactly 5 seconds into the `accumulated_buffer` to calculate its cumulative resonance score.

## 9. Analysis Context Window (Unified 15.2s Model)
*   **Resolution**: 15,200ms (15.2 Seconds)
*   **Implementation**: The historical context maintained by the C core (15s context + 200ms lookahead). This ensures that the resonance lookback and adaptive thresholding operate with full parity between real-time and offline analysis.

## 10. Rolling Threshold & State Cleanup
*   **Resolution**: 15,000ms (15 Seconds)
*   **Implementation**:
    *   **Thresholding**: The window size for the adaptive rolling midpoint used to identify peaks.
    *   **Memory Management**: Snapshots older than 15 seconds are removed from the internal state to maintain a stable memory footprint.

## 11. Global Circular Buffer
*   **Resolution**: 60,000ms (60 Seconds)
*   **Implementation**: The maximum audio history maintained in RAM by the `analyze~` object for background analysis retrieval.

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
| **Context Window** | 15,200.0| Integration | Max MSP |
| **Threshold Sub-Window** | 999.0 | Logic | C Core |
| **State Cleanup** | 15,000.0 | Management | C Core |
| **Global Buffer** | 60,000.0 | Storage | Max MSP |

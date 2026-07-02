# Spectral Analysis Pipeline Report: Data Flow and Orchestration

This report clarifies the architectural relationship between the core C transience algorithm (`cumulative_transience.c`) and its host environments (Max and Python), specifically addressing the flow of spectral data and peak detection.

## 1. Unified C Core: The Source of Truth

**The spectral analysis (FFT, Mel-filtering, Spectral Flux, and Peak Detection) occurs entirely within the C core.**

The distinction between "Incremental" and "Batch" analysis has been eliminated within the C library itself. Both paths now utilize the same stateful `TransientAnalyzer` engine to ensure bit-perfect synchronization:

1.  **Windowing & FFT**: Converts raw time-domain audio into the frequency domain using a 2048-point Hanning window.
2.  **Mel-Filtering**: Aggregates FFT bins into 128 Mel-spaced bands.
3.  **Spectral Flux**: Calculates the rate of change in energy for each band.
4.  **Stateful Resonance**: Tracks historical transient energy in a 5-second buffer and calculates resonance qualifiers.
5.  **Peak Detection**: Identifies transients using a 15-second rolling median threshold, 200ms suppression, and an adaptive median-based prominence check (`prom > median`).

## 2. Host Orchestration: Max vs. Python

The "Incremental" and "Batch" labels now refer only to how the host environment interacts with the C core, not to the underlying algorithm.

### Incremental Mode (Max External / Real-Time)
- **Flow**: The host hands the C core a small block of audio (typically 100ms) as it arrives in real-time.
- **State**: The `TransientAnalyzer` maintains internal buffers for FFT overlap, spectral history, and resonance energy.
- **Output**: The C core returns peaks and metrics as they are finalized (after the 200ms lookahead period).

### Batch Mode (Python Visualizer / Offline)
- **Flow**: The host hands the C core the entire audio file.
- **Implementation**: The C core internally simulates the incremental process by stepping through the file in 100ms chunks.
- **Output**: Returns a `FullAnalysisResult` containing the entire flux envelopes, the list of all `PeakResult` objects (including snapshots and qualifiers), and frame-by-step metrics.

---

## 3. Achieved Evolution: Unified Windowing

The system utilizes a **Unified 15.2s Sliding Window** (15s context + 200ms lookahead) to harmonize the two environments.

### Implementation of Synchronization
1.  **Global Peak Sorting**: The C core extracts peaks across all 4 bands and performs a **global chronological sort** before calculating resonance. This ensures that inter-band timing is identical in both real-time and offline analysis.
2.  **Shared Memory Layout**: The `PeakResult` and `ChunkAnalysisResult` structures are shared between C, the Max external, and the Cython extension, preventing memory alignment errors.
3.  **Deterministic State**: Because the "Batch" engine internally uses the "Incremental" engine, the rolling threshold convergence and resonance qualifiers are guaranteed to match.

---

## 4. Scaling and Visualization

A critical goal of the unification was to ensure that "Flux" (the onset strength) matches the Y-axis of the visual graphs and the console output of the Max object.

### Diagnostic Results
As of the current implementation, the scaling has been verified using `diagnose_scaling.py`:
- **Flux / Onset Strength**: Represented as raw spectral change (un-normalized).
- **Max Peak**: The `max_peak_value` is dynamically tracked across the entire session to scale visual snapshots.
- **Ratio (Inc/Batch)**: **1.0000**. The values seen in the Max console are identical to the spikes on the Python-rendered graph.

### Visualization Highlights
- **Transient Plot**: Y-axis represents raw flux.
- **Thresholds**: Displayed as moving averages directly calculated from the plotted spikes.
- **Snapshot Bar**: Represents scores calculated from the product of Flux and Resonance Qualifiers, dynamically scaled to the `min_score_seen` and `max_score_seen` of the session.

## 5. Conclusion
The "back-and-forth" and divergent pipelines have been replaced by a **Unified C-Managed Orchestration**. The C core is now the single source of truth for both real-time performance and high-fidelity visualization, with host environments acting only as data providers and result consumers.

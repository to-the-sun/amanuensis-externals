# Spectral Analysis Pipeline Report: Data Flow and Orchestration

This report details the architectural relationship between the core C transience algorithm (`cumulative_transience.c`) and its host environments (Max and Python).

## 1. Unified C Core: The Source of Truth

**The spectral analysis (FFT, Mel-filtering, Spectral Flux, and Peak Detection) occurs entirely within the C core.**

The distinction between "Incremental" and "Batch" analysis is handled at the orchestration level, while the underlying DSP logic remains unified:

1.  **Windowing & FFT**: Converts raw time-domain audio into the frequency domain using a 2048-point Hanning window.
2.  **Mel-Filtering**: Aggregates FFT bins into 128 Mel-spaced bands.
3.  **Spectral Flux**: Calculates the rate of change in energy for each band.
4.  **Stateful Resonance**: Tracks historical transient energy in a 5-second buffer and calculates resonance qualifiers.
5.  **Peak Detection**: Identifies transients using a 15-second context, 200ms suppression, and an adaptive midpoint-based prominence check (`prom > midpoint`).

## 2. Host Orchestration: Max vs. Python

"Incremental" and "Batch" refer to how the host environment interacts with the C core.

### Incremental Mode (Max External / Real-Time)
- **Flow**: The host hands the C core a small block of audio (typically 100ms) as it arrives in real-time.
- **State**: The `TransientAnalyzer` maintains internal buffers for FFT overlap, spectral history, and resonance energy.
- **Output**: The C core returns peaks and metrics as they are finalized (after the 200ms lookahead period).

### Batch Mode (Python Visualizer / Offline)
- **Flow**: The host hands the C core the entire audio file.
- **Implementation**: The C core (`analyzer_batch_analyze`) internally simulates the incremental process by stepping through the file in 100ms chunks, ensuring parity with real-time performance.
- **Output**: Returns a `FullAnalysisResult` containing the entire flux envelopes, the list of all `PeakResult` objects, and frame-by-step metrics.

## 3. Current Implementation: Unified Windowing

The system utilizes a **Unified 15.2s Sliding Window** (15s context + 200ms lookahead) to harmonize the two environments.

### Implemented Synchronization
1.  **Global Peak Sorting**: The C core extracts peaks across all 4 bands and performs a **global chronological sort** before calculating resonance. This ensures that inter-band timing and resonance accumulation are identical in both real-time and offline analysis.
2.  **Shared Memory Layout**: The `PeakResult` and `ChunkAnalysisResult` structures are shared between C, the Max external, and the Cython extension.
3.  **Deterministic State**: Because the "Batch" engine internally uses the same incremental logic, the rolling threshold convergence and resonance qualifiers match between environments.

## 4. Scaling and Visualization

A critical goal of the unification was ensuring that "Flux" (the onset strength) matches the Y-axis of the visual graphs and the console output of the Max object.

### Current Metrics
- **Flux / Onset Strength**: Represented as raw spectral change (average across 32 frequency bins).
- **Max Peak**: The `max_peak` value is dynamically tracked across the entire session to scale visual snapshots.
- **Ratio (Inc/Batch)**: **1.0000**. The values seen in the Max console are identical to the spikes on the Python-rendered graph.

### Visualization Standards
- **Transient Plot**: Y-axis represents raw flux.
- **Thresholds**: Displayed as moving averages (midpoints) directly calculated from the plotted spikes.
- **Snapshot Bar**: Represents scores calculated from the product of Flux and Resonance Qualifiers.

## 5. Conclusion
The pipeline is managed by a **Unified C-Managed Orchestration**. The C core is the single source of truth for both real-time performance and high-fidelity visualization, with host environments acting as data providers and result consumers.

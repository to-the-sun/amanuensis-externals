# Spectral Analysis Pipeline Report: Data Flow and Orchestration

This report clarifies the architectural relationship between the core C transience algorithm (`cumulative_transience.c`) and its host environments (Max and Python), specifically addressing the flow of spectral data and peak detection.

## 1. Unified C Core: The Source of Truth

**The spectral analysis (FFT, Mel-filtering, Spectral Flux, and Peak Detection) occurs entirely within the C core.**

The algorithmic distinction between "Incremental" and "Batch" analysis has been eliminated. Both paths now utilize the same stateful `TransientAnalyzer` engine:

1.  **Windowing & FFT**: Converts raw audio into the frequency domain using a 2048-point Hanning window.
2.  **Mel-Filtering**: Aggregates FFT bins into 128 Mel-spaced bands.
3.  **Spectral Flux**: Calculates the rate of change in energy, normalized by the number of bins (32 per band).
4.  **Stateful Resonance**: Tracks historical transient energy in a 5-second buffer and calculates resonance qualifiers with a 99ms exclusion zone.
5.  **Peak Detection**: Identifies transients using a dynamic historical peak-based rolling midpoint threshold and adaptive prominence.

## 2. Host Orchestration: Max vs. Python

The "Incremental" and "Batch" labels now refer only to how the host environment interacts with the C core.

### Incremental Mode (Max External / Real-Time)
- **Flow**: The host hands the C core a 100ms block of audio as it arrives.
- **State**: The `TransientAnalyzer` maintains internal caches for FFT overlap, spectral history (15.2s), and resonance energy (5s).
- **Output**: The C core returns peaks and metrics as they are finalized (after the 200ms lookahead).

### Batch Mode (Python Visualizer / Offline)
- **Flow**: The host hands the C core the entire audio file (or large segments).
- **Implementation**: The C core internally simulates the incremental process by stepping through the file in 100ms chunks to ensure deterministic parity with the real-time engine.
- **Output**: Returns a result containing the entire flux envelopes, the list of all `PeakResult` objects, and frame-by-step metrics.

---

## 3. Achieved Evolution: Unified Windowing

The system utilizes a **Unified 15.2s Sliding Window** model to harmonize the two environments.

### Implementation of Synchronization
1.  **Global Peak Sorting**: The C core extracts peaks across all 4 bands and performs a **global chronological sort** before resonance calculation.
2.  **Shared Memory Layout**: Structures are shared between C, Max, and Cython, preventing alignment errors.
3.  **Deterministic State**: Because the "Batch" engine internally uses the "Incremental" logic, the rolling threshold convergence and resonance qualifiers are guaranteed to match.

---

## 4. Scaling and Visualization

A critical goal of the unification was ensuring that "Flux" matches the Y-axis of visual graphs and Max console output.

### Current Implementation Results
- **Flux / Onset Strength**: Represented as raw spectral change (average dB change per bin).
- **Max Peak**: Dynamically tracked across the session to scale visual snapshots.
- **Ratio (Inc/Batch)**: **1.0000**. Values in the Max console are identical to the spikes on the Python-rendered graph.

### Visualization Highlights
- **Transient Plot**: Y-axis represents raw flux.
- **Thresholds**: Displayed as moving midpoints directly calculated from the plotted spikes.
- **Snapshot Bar**: Represents scores calculated from the product of Flux and Resonance Qualifiers, dynamically scaled to the session's score range.

## 5. Conclusion
The divergent pipelines have been replaced by a **Unified C-Managed Orchestration**. The C core is the single source of truth for both real-time performance and high-fidelity visualization, with host environments acting as data providers and result consumers.

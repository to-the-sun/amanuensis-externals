# Cumulative Transience Pipeline: Scaling and Unification Report

## 1. Initial State of the Code (Historical)
At the beginning of this unification process, the `cumulative_transience` analysis pipeline suffered from significant architectural and mathematical discrepancies between its two primary modes: **Batch** (used for drawing graph lines in the visualizer) and **Incremental** (used for real-time console output and threshold calculation).

### The "Two-Mode" Problem:
The C core previously contained two distinct entry points for analysis: `analyzer_push_audio` (Incremental/Real-time) and `analyzer_analyze_audio` (Batch/Offline).

Historically, these modes diverged over time:
*   **Incremental** was designed for the stateful, block-by-block processing required by the Max external (`analyze~`).
*   **Batch** was created as a high-performance "all-at-once" pass for the Python visualizer, but it did not share the same stateful logic as the incremental pass.

This violated the core design principle that the **C core should handle the DSP logic for real-time only**, while any "batch" behavior should be an orchestration performed by the host by calling the real-time functions in sequence.

### Key Historical Discrepancies:
*   **Memory Interface Mismatch:** The Cython extension had an incomplete and misaligned definition of the `TransientAnalyzer` C struct, causing memory corruption in the Incremental path when called from Python.
*   **Floor "Breathing" Artifact:** The Batch analyzer calculated its 80dB noise floor using a local rolling maximum that reset for every file, causing artificial suppression of transients.
*   **Lack of Normalization:** The FFT implementation was unnormalized, resulting in power values amplified by a factor of $N^2$.
*   **Inconsistent Lookahead:** The incremental analyzer lacked sufficient lookahead context at chunk boundaries, causing periodic synchronization "seams."

## 2. Unification and Resolution

The entire pipeline has been unified and brought into alignment with a standardized mathematical model that prioritizes the real-time engine as the source of truth.

### Pipeline Unification:
*   **Standardized Normalization**: Added explicit $1/N^2$ normalization to the power spectral density calculation. Mel energy values are now in a physically meaningful range (approx. $-100$ to $0$ dB).
*   **Synchronized Floor Logic**: Both analysis paths now follow the exact same execution order: calculating Mel energy, updating the persistent rolling maximum, applying the 80dB floor, and calculating flux.
*   **Opaque Pointer Interface**: The Cython extension now treats the `TransientAnalyzer` as an opaque pointer, preventing memory layout mismatches.
*   **Stateful Orchestration**: The batch analyzer has been refactored to internally mirror the stateful behavior of the real-time engine frame-by-frame.

### Scaling Consistency:
*   **Bin-Average Restored**: Both pipelines now strictly use the **average across the 32 frequency bins** (`flux / 32.0`) for Onset Strength.
*   **Visual Alignment**: Fixed memory layout and floor convergence ensure that visual "spikes" on the graph match the flux magnitudes reported in the console.

## 3. Current State
The **Batch analyzer is now in perfect alignment with the Incremental analyzer**. While `analyzer_batch_analyze` remains available for performance, it internally utilizes the same stateful logic as the real-time engine. The real-time logic is the unified standard for the system, and the offline pass simulates that behavior with deterministic accuracy.

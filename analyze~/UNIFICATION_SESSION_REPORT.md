# Cumulative Transience Pipeline: Scaling and Unification Report

## 1. Initial State of the Code
At the beginning of this session, the `cumulative_transience` analysis pipeline suffered from significant architectural and mathematical discrepancies between its two primary modes: **Batch** (used for drawing graph lines in the visualizer) and **Incremental** (used for real-time console output and threshold calculation).

### The "Two-Mode" Problem:
The C core contained two distinct entry points for analysis: `analyzer_push_audio` (Incremental/Real-time) and `analyzer_analyze_audio` (Batch/Offline).

Historically, these modes existed because:
*   **Incremental** was designed for the stateful, block-by-block processing required by the Max external (`analyze~`).
*   **Batch** was created as a high-performance "all-at-once" pass for the Python visualizer and library migration tools, allowing them to calculate envelopes for an entire file in a single function call.

However, because these were implemented as separate logic paths within the C core, they diverged over time. This violated the core design principle that the **C core should handle the DSP logic for real-time only**, while any "batch" behavior should be an orchestration performed by the host (Python or Max) by calling the real-time functions in sequence.

### Key Discrepancies:
*   **Memory Interface Mismatch:** The Cython extension (`ct_extension.pyx`) had an incomplete and misaligned definition of the `TransientAnalyzer` C struct. This caused the C code to read incorrect memory addresses for critical data like Mel filters and FFT windows when called from Python, resulting in garbage Mel energy values in the Incremental path.
*   **Floor "Breathing" Artifact:** The Batch analyzer calculated its 80dB noise floor using a locally-updated rolling maximum that was reset for every file. If a loud transient occurred, the floor for that specific frame would spike upwards, "cutting off" the bottom of the transient and artificially suppressing the visual spikes on the graph.
*   **Lack of Normalization:** The custom Radix-2 FFT implementation was unnormalized, resulting in power values amplified by a factor of $N^2 \approx 4.2 \times 10^6$. This made the Mel energy values extremely large and highly sensitive to small variations in the noise floor.
*   **Inconsistent Lookahead:** The incremental analyzer was missing sufficient lookahead context for the 2048-sample FFT at chunk boundaries, causing periodic synchronization "seams."

## 2. Changes Made During This Session
The entire pipeline has been unified and brought into alignment with a standardized mathematical model that prioritizes the real-time engine as the source of truth.

### Pipeline Unification:
*   **Standardized Normalization:** Added explicit $1/N^2$ normalization to the power spectral density calculation in both paths. Mel energy values are now in a physically meaningful range (approx. $-100$ to $0$ dB).
*   **Synchronized Floor Logic:** Both analysis paths now follow the exact same execution order:
    1. Calculate Mel energy for the current frame.
    2. Update the persistent rolling maximum energy.
    3. Calculate the 80dB noise floor relative to that **updated maximum**.
    4. Apply clamping and calculate the spectral flux.
    This eliminates the "breathing" artifact and ensures the Batch pass produces spikes that match the magnitude seen in real-time.
*   **Opaque Pointer Interface:** The Cython extension was updated to treat the `TransientAnalyzer` as an opaque pointer. This prevents any future memory layout mismatches between Python and C.
*   **Memory Safety:** Fixed a critical memory leak in the incremental analyzer where the temporary audio processing buffer was being allocated but never freed.

### Scaling Consistency:
*   **Bin-Average Restored:** Both pipelines now strictly use the **average across the 32 frequency bins** (`flux / 32.0`) for Onset Strength.
*   **Visual Alignment:** By fixing the memory layout and floor convergence issues, the visual "spikes" on the graph lines are now correctly scaled to match the flux magnitudes reported in the console.

## 3. Summary of Synchronization
The **Batch analyzer was brought into line with the Incremental analyzer**. While `analyzer_analyze_audio` remains in the C core as a performance optimization for the Python visualizer, it has been refactored to internally mirror the stateful behavior of the real-time engine frame-by-frame. The real-time logic is now the unified standard for the system, and the offline pass simulates that behavior with deterministic accuracy.

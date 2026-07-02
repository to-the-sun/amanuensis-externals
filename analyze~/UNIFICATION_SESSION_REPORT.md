# Unification Session Report: Scaling and Parity

This report documents the session that unified the `cumulative_transience` analysis pipeline, resolving discrepancies between the Max External (`analyze~.c`) and the Python Visualizer (`analyze_files.py`).

## 1. Initial State of the Code (Historical)
Previously, the pipeline suffered from architectural and mathematical discrepancies between **Batch** (offline) and **Incremental** (real-time) modes.

### The "Two-Mode" Problem:
- **Discrepancy**: The C core originally contained two distinct entry points (`analyzer_push_audio` and `analyzer_analyze_audio`) that diverged over time.
- **Memory mismatch**: The Cython extension had an misaligned definition of the C struct, causing corrupted memory reads.
- **Normalization mismatch**: Unnormalized FFT results led to inflated Mel energy values.
- **Inconsistent Lookahead**: Missing lookahead at chunk boundaries caused synchronization "seams" in real-time.

## 2. Resolved Challenges during Unification
The pipeline has been unified under a standardized mathematical model where the real-time engine is the source of truth.

### Implemented Improvements:
- **Standardized Normalization**: Added explicit $1/N^2$ normalization to the power spectral density calculation.
- **Synchronized Floor Logic**: Both paths now utilize the same 80dB noise floor relative to a stateful rolling maximum.
- **Opaque Pointer Interface**: The Cython extension now treats the `TransientAnalyzer` as an opaque pointer, preventing memory layout mismatches.
- **Memory Safety**: Fixed critical leaks in the incremental analyzer.
- **Scaling Consistency**: Both pipelines strictly use the average across frequency bins (`flux / 32.0`) for Onset Strength, ensuring visual alignment between spikes on the graph and reported console values.

## 3. Final State: Synchronized Operation
The **Batch analyzer has been brought into perfect alignment with the Incremental analyzer**. While `analyzer_batch_analyze` remains available for performance, it internally mirrors the stateful behavior of the real-time engine frame-by-frame. This ensures that the offline pass simulates real-time behavior with deterministic accuracy.

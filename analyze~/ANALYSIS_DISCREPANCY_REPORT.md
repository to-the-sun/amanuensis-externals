# Analysis Discrepancy Report: Resolved

## Executive Summary
Discrepancies in the output of the `analyze~` Max object compared to the Python reference implementation have been fully resolved through the **unification of the C core pipeline**. Both environments now use the exact same stateful engine, eliminating algorithmic divergence.

## Technical Resolution

### 1. Unified 15.2s Stateful Engine
The distinction between "batch" (Python) and "incremental" (Max) logic has been removed from the C library (`cumulative_transience.c`).
- **Unified Logic**: Both environments call the same `analyzer_analyze_chunk` function.
- **Deterministic State**: The `TransientAnalyzer` maintains a 15.2-second context window, ensuring that rolling thresholds and resonance buffers are identical whether processed in real-time or offline.

### 2. Synchronized Memory Layout
Previously, the Cython extension and the C core had mismatched struct definitions (e.g., `PeakResultList` size), causing corruption and 10x-100x scaling errors in reported metrics.
- **Resolution**: Structures in `cumulative_transience.h` and `ct_extension.pyx` are now perfectly synchronized.

### 3. Scaling and Normalization Parity
- **Flux (Onset Strength)**: Confirmed as the same metric across all systems.
- **Max Peak Tracking**: Both systems dynamically update a `max_peak` reference to ensure normalization consistency.
- **Thresholds**: Moving averages are calculated from the raw flux spikes in both environments.
- **Verification**: `diagnose_scaling.py` confirms a **1.0000 ratio** between Max-style chunked analysis and Python-style batch analysis.

### 4. Precision Timing
- **Frame Duration**: Calculated precisely as `1000.0 * hop_size / sample_rate` to prevent drift over time.
- **Video Sync**: Python renders use a true 30 FPS clock synchronized to the analysis timeline via `np.searchsorted`.

## Conclusion
The analysis pipeline is now fully harmonized. The "Incremental" vs "Batch" distinction exists only at the host-orchestration level, while the underlying DSP and transience detection are bit-perfect across both Max and Python.

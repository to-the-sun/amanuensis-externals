# Analysis Discrepancy Report: Resolved

## Executive Summary
Discrepancies in the output of the `analyze~` Max object compared to the Python reference implementation have been fully resolved through the **unification of the C core pipeline**. Both environments now utilize the same stateful engine, eliminating algorithmic divergence.

## Technical Resolution

### 1. Unified 15.2s Stateful Engine
The distinction between "batch" (Python) and "incremental" (Max) logic has been moved from the algorithm to the orchestration.
- **Unified Logic**: Both environments utilize the same underlying C functions for spectral analysis and peak detection.
- **Deterministic State**: The `TransientAnalyzer` maintains a 15.2-second context window, ensuring that rolling thresholds and resonance buffers are identical whether processed in real-time or offline.

### 2. Synchronized Memory Layout
- **Resolution**: Structures in `cumulative_transience.h` and `ct_extension.pyx` are now perfectly synchronized. The use of an opaque pointer for the `TransientAnalyzer` in Cython prevents memory layout corruption.

### 3. Scaling and Normalization Parity
- **Flux (Onset Strength)**: Confirmed as the identical metric across all systems (average flux across frequency bins).
- **Max Peak Tracking**: Both systems dynamically update a `max_peak` reference in the C core to ensure normalization consistency.
- **Thresholds**: Midpoint-based thresholds are calculated from the same flux spikes in both environments.
- **Verification**: `diagnose_scaling.py` (historical check) confirmed a **1.0000 ratio** between Max-style chunked analysis and Python-style batch analysis.

### 4. Precision Timing
- **Frame Duration**: Calculated precisely based on the sample rate (`1000.0 * hop_size / sr`) to prevent temporal drift.
- **Video Sync**: Python renders use a 30 FPS clock precisely mapped to the analysis timeline.

## Conclusion
The analysis pipeline is now fully harmonized. Parity has been achieved between the real-time performance of the Max external and the high-fidelity reporting of the Python visualizer.

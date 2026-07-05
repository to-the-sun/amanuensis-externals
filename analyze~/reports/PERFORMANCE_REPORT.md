# Performance Report: Sliding Window Analysis Bottlenecks and Resolution

This report analyzes the performance challenges encountered during the implementation of the **Unified 15.2s Windowing Model** and details the optimizations that were achieved to ensure real-time feasibility.

## 1. Historical Problem: Redundant Computation

Before the implementation of the Incremental Cache Model, the system suffered from significant computational inefficiency. The primary bottleneck was the **Redundancy Factor**.

### The Math of Inefficiency (Historical)
- **Step Size**: 100ms
- **Window Size**: 15.2s (15,200ms)
- **Redundancy**: 15,200 / 100 = **152x**

In the original stateless model, for every 100ms of new audio, the system re-analyzed the entire preceding 15.2 seconds. Every audio sample was processed by the STFT, Mel-filterbank, and Spectral Flux algorithms approximately 152 times.

## 2. Historical Technical Causes

### A. Stateless Spectral Pipeline
The initial `analyzer_analyze_audio` implementation was stateless. It did not remember the spectrogram from the previous 100ms hop, necessitating a full re-calculation of the FFT for the entire 15.2s buffer on every call.

### B. Memory Allocation Overhead
The "Full Orchestration" model previously performed multiple `malloc` and `free` operations per 100ms chunk for linearized audio, spectrogram buffers, and result structures. This added measurable latency and cache pressure.

### C. Log-Power and Normalization Pass
The original spectral pipeline included a global normalization pass for each 15.2s window, requiring multiple iterations over the entire spectrogram buffer after the FFT and Mel-filtering were complete.

## 3. Optimization Strategies Implemented

To restore performance without sacrificing synchronization, the C core evolved from a **Batch-per-Chunk** model to the current **Incremental Cache Model**.

### Strategy 1: Spectral Caching (The "Rolling Spectrogram")
The `TransientAnalyzer` now maintains a persistent spectral buffer.
- **Input**: The host hands only the **newest 100ms** of audio.
- **Process**: The C core calculates the FFT/Mel-bins for just that 100ms and appends them to a circular spectral buffer.
- **Result**: FFT complexity dropped from O(Window) to O(Hop), yielding a **~150x speedup** in the spectral stage.

### Strategy 2: Pre-allocated State
The `TransientAnalyzer` moved away from temporary allocations.
- Large buffers for the spectrogram, envelopes, and thresholds are allocated once during `analyzer_create`.
- Persistent result buffers are used to eliminate `malloc` churn during the analysis loop.

### Strategy 3: Threshold and Metric Optimization
The 15-second rolling threshold and the 5-second resonance buffer utilize incremental logic (adding new data and subtracting old data). The "Active Zone" logic distinguishes between the *context* (used for normalization) and the *action* (where peaks are emitted).

## 4. Current State: Achieved Optimization

The performance bottleneck has been resolved by the **Incremental Cache Model**:

1.  **Spectral Caching (O(Hop) FFT)**: The `TransientAnalyzer` maintains a persistent circular buffer for the Mel spectrogram and flux envelopes.
2.  **Persistent Allocation**: Large buffers and filters are allocated once during initialization, eliminating `malloc` churn.
3.  **Linearized Context**: Peak detection and resonance calculations still utilize the full 15.2s context by linearizing only the necessary segments from the internal cache.

## 5. Conclusion

The implementation of **Spectral Caching** reduced the complexity of the spectral pipeline from O(Window) to O(Hop). This optimization ensures that the **Unified 15.2s Windowing Model** remains computationally feasible for both real-time Max usage and efficient Python batch processing while maintaining perfect synchronization.

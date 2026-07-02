# Performance Report: Incremental Processing Model

This report analyzes the performance evolution of the spectral analysis pipeline, from early bottlenecks to the current optimized state.

## 1. Past Problem: Redundant Computation

Initial implementations of the **Unified 15.2s Windowing Model** achieved perfect synchronization between Max and Python, but at a significant computational cost due to redundancy.

### The Inefficiency (Historical)
- **Step Size**: 100ms
- **Window Size**: 15.2s (15,200ms)
- **Redundancy**: 15,200 / 100 = **152x**

For every 100ms of new audio, the system re-analyzed the preceding 15 seconds. This meant every single audio sample was processed approximately 152 times, leading to massive CPU overhead.

## 2. Past Technical Causes

### A. Stateless Spectral Pipeline
The `analyzer_analyze_audio` function was originally stateless, requiring a full re-calculation of the STFT for the entire 15.2s buffer on every 100ms call.

### B. Memory Allocation Overhead
Frequent `malloc` and `free` operations per 100ms chunk created measurable latency and cache pressure.

### C. Normalization Pass
Global normalization required multiple iterations over the entire spectrogram buffer after the FFT and Mel-filtering were complete.

## 3. Current State: Incremental Processing Model

The performance bottleneck has been resolved through the implementation of an **Incremental Cache Model**, which is now the standard for the `cumulative_transience` library.

### Implemented Optimizations:

1.  **Spectral Caching (O(Hop) FFT)**: The `TransientAnalyzer` maintains a persistent circular buffer for the Mel spectrogram and flux envelopes. Instead of re-calculating the STFT for 15.2s of audio, the system only processes the **NEW 100ms hop** and appends it to the cache. FFT complexity has dropped from O(Window) to O(Hop), yielding a ~150x speedup in the spectral stage.
2.  **Persistent Allocation**: Large buffers and filters are allocated once during `analyzer_create`, eliminating `malloc` churn during the analysis loop.
3.  **Linearized Context**: Peak detection and resonance calculations still utilize the full 15.2s context by linearizing only the necessary segments from the internal cache.

## 4. Conclusion

The implementation of **Spectral Caching** and **Persistent Allocation** ensures that the **Unified 15.2s Windowing Model** remains computationally feasible for both real-time Max usage and efficient Python batch processing. The system now provides bit-perfect synchronization without the redundant processing overhead of earlier versions.

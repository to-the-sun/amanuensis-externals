# Performance Report: Sliding Window Analysis Bottlenecks

This report analyzes the performance degradation observed following the implementation of the **Unified 15.2s Windowing Model** and proposes strategies for optimization.

## 1. Problem Identification: Redundant Computation

The current implementation achieves perfect synchronization between Max and Python, but at a significant computational cost. The primary bottleneck is the **Redundancy Factor**.

### The Math of Inefficiency
- **Step Size**: 100ms
- **Window Size**: 15.2s (15,200ms)
- **Redundancy**: 15,200 / 100 = **152x**

For every 100ms of new audio, the system re-analyzes the preceding 15 seconds. This means every single audio sample is processed by the STFT, Mel-filterbank, and Spectral Flux algorithms approximately 152 times. In a 3-minute song, the system is performing the work of analyzing ~7.5 hours of audio.

## 2. Technical Causes

### A. Stateless Spectral Pipeline
The `analyzer_analyze_audio` function (called by `analyzer_analyze_chunk`) is stateless. It does not remember the spectrogram from the previous 100ms hop. Consequently, it must re-calculate the FFT for the entire 15.2s buffer on every call.

### B. Memory Allocation Overhead
The current "Full Orchestration" model in C performs multiple `malloc` and `free` operations per 100ms chunk:
1.  Linearized audio buffer (Host-side)
2.  Spectrogram buffer (C-side)
3.  Envelope and Threshold buffers for 4 bands (C-side)
4.  Peak reference and index arrays (C-side)
5.  `ChunkAnalysisResult` (Host-side, heap-allocated for stack safety)

While modern OS allocators are fast, doing this 10 times per second for large spectral buffers adds measurable latency and cache pressure.

### C. Log-Power and Normalization Pass
The spectral pipeline includes a global normalization pass (`max_db`) for each 15.2s window. This requires iterating over the entire spectrogram buffer multiple times after the FFT and Mel-filtering are complete, further increasing the O(N) complexity of the chunk analysis.

## 3. Proposed Alleviation: Incremental Processing Model

To restore performance without sacrificing synchronization, the C core should evolve from a **Batch-per-Chunk** model to an **Incremental Cache** model.

### Strategy 1: Spectral Caching (The "Rolling Spectrogram")
Instead of handing the C core 15.2s of raw audio, the system should maintain a persistent spectral buffer within the `TransientAnalyzer` struct.
- **Input**: The host hands only the **newest 100ms** of audio.
- **Process**: The C core calculates the FFT/Mel-bins for just that 100ms and appends them to a circular spectral buffer.
- **Result**: FFT complexity drops from O(Window) to O(Hop). This would theoretically yield a **~150x speedup** in the spectral stage.

### Strategy 2: Pre-allocated State
The `TransientAnalyzer` should move away from temporary allocations.
- Large buffers for the spectrogram, envelopes, and thresholds should be allocated once during `analyzer_create`.
- The `ChunkAnalysisResult` can be passed as a pointer to a persistent member of the analyzer or a pre-allocated host-side structure, eliminating `malloc` churn.

### Strategy 3: Threshold and Metric Optimization
The 15-second rolling threshold and the 5-second resonance buffer already utilize incremental logic (adding new data and subtracting old data). This pattern should be extended to the entire pipeline. The "Active Zone" logic already prepares the way for this by distinguishing between the *context* (used for normalization) and the *action* (where peaks are actually emitted).

## 4. Conclusion

The current performance penalty is a direct result of "brute-forcing" the sliding window to ensure mathematical parity between Max and Python. By implementing **Spectral Caching** and **Persistent Allocation**, we can maintain this parity while reducing the CPU load to a level comparable to the original 6s batch implementation, effectively making the 15.2s window "free" in terms of incremental cost.

# Transience Implementation Report: Direct Source Integration vs. DLL

## Overview
This report outlines the necessity for maintaining `cumulative_transience.h` and `cumulative_transience.c` within the `analyze~` object directory, even though a compiled `libtransience.dll` was previously provided. The object has been transitioned to **static linking**, where the analysis logic is compiled directly into the `analyze~.mxe64` binary.

## Why the DLL Alone was Insufficient

While a DLL provides the compiled logic, it has several limitations for high-performance, real-time audio analysis:

1.  **State Persistence and Caching**: To support the new **20ms analysis hop** (increased from 100ms), the object needs to be extremely efficient. Recomputing the Mel filterbanks and window functions every 20ms is computationally expensive. By linking the source directly, we can modify the `TransientAnalyzer` structure to include persistent cache members (`cached_mel_filters`, `cached_window`). This allows the analyzer to maintain its own internal state across analysis frames, only recomputing these resources if parameters like the sample rate change.
2.  **Function Call Overhead**: Calling functions across a DLL boundary via pointers (`GetProcAddress`) involves a small amount of overhead. While negligible for occasional tasks, this overhead accumulates when performing spectral analysis every 20ms. Static linking allows for direct, inlined-capable function calls and better compiler optimization (LTO - Link Time Optimization).
3.  **Deployment and Stability**: Relying on an external DLL requires the system to manage paths and ensure the DLL version matches the object's expectations. Integrating the source directly simplifies the object into a single, standalone `.mxe64` file, reducing the risk of "DLL not found" errors or version mismatches during runtime.
4.  **Signature Flexibility**: To implement the caching mentioned above, the signature of core functions (like `analyzer_analyze_audio`) needed to be updated to accept the `TransientAnalyzer` state pointer. Static linking makes these architectural improvements straightforward, whereas updating a DLL interface requires rebuilding both the library and the host object.

## Conclusion
Directly utilizing the `cumulative_transience.h` and `.c` files allows the `analyze~` object to meet the user's requirement for "immediate" event-based output through significantly improved performance and persistent state management, which would have been impossible or significantly more complex using the previous DLL-only approach.

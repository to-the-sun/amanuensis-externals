# Peak Distance Comparison: C vs. Python (SciPy)

This report compares the peak detection strategies used in the C core engine and the original Python (SciPy) implementation.

## 1. Strategy Overview

### C Core (Sequential Greedy)
The C engine uses a single-pass, sequential algorithm designed for real-time DSP.
- **Logic**: It scans the flux envelope linearly. When a candidate peak is found, it checks if it's within 200ms of the *previously accepted* peak.
- **Replacement**: If a new candidate is within the 200ms window and has higher energy than the existing peak in that window, it **replaces** it.

### Python / SciPy (Global Priority)
The original SciPy-based implementation (`scipy.signal.find_peaks`) uses a multi-pass approach.
- **Logic**: It identifies all local maxima first, then sorts them by magnitude. It accepts the highest peaks first and invalidates any neighbors within the specified distance (200ms).

## 2. Behavioral Differences

The primary difference occurs in dense transient clusters:
- **C Strategy**: Might pick a slightly lower peak if it arrives later in the scan, provided it "wins" the 200ms competition against a previous peak. It is optimized for low-latency, streaming decisions.
- **Python/SciPy Strategy**: Always prioritizes the absolute highest energy peaks in a global window, but requires buffering the entire signal.

## 3. Technical Constants
Despite different strategies, the fundamental constants are aligned for parity:
- **Temporal Resolution**: 1ms frames.
- **Distance Threshold**: 200ms.
- **Prominence**: Both use a prominence check relative to the local background (midpoint in C, 0.5/relative in Python).

## 4. Comparison Summary

| Feature | Python (SciPy Reference) | C (Current Engine) |
| :--- | :--- | :--- |
| **Algorithm** | Multi-pass (Global) | Single-pass (Sequential) |
| **Strategy** | Global Priority (Highest wins) | Latest/Highest wins (within window) |
| **Optimization** | Mathematical Optimality | Real-time Streaming |
| **Complexity** | O(N log N) | O(N) |

## 5. Conclusion
The C engine's sequential strategy is essential for the `analyze~` Max object and the real-time simulation in Python. While subtle differences may occur in extremely dense transient areas compared to a global SciPy view, the 200ms suppression effectively replicates the *intent* of the original algorithm while enabling low-latency performance.

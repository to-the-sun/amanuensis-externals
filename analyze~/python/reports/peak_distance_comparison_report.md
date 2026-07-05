# Peak Distance Implementation Comparison: Historical SciPy vs. Current C-Core

This report details the evolution of the **Minimum Peak Distance** logic from the original Python reference to the current high-performance C engine (`cumulative_transience.c`).

## 1. Evolution of the Implementation

Historically, the project utilized two different approaches for peak detection:

*   **Original Python Reference**: Utilized `scipy.signal.find_peaks(..., distance=200, prominence=0.5)`. This was a **multi-pass, global optimization** algorithm that prioritized the mathematically highest peaks across the entire signal.
*   **Current C-Core (Unified)**: Implements a **single-pass, greedy linear scan**. This approach is now used in **both** the Max External (`analyze~`) and the Python Visualizer (`analyze_files.py`) to ensure perfect synchronization.

## 2. Selection Strategy: "Knock-out" Logic

The current C-core logic handles peak competition (multiple peaks within a 200ms window) sequentially:

- **Sequential Replacement**: The C engine uses a greedy "replace if higher" logic.
- If a new peak (B) is found within 200ms of the previous peak (A), and B is higher than A, B **replaces** A.
- This approach was chosen to support real-time streaming, where the full signal is not available for global sorting.

## 3. Comparison with Historical SciPy Strategy

| Feature | Historical Python (SciPy) | Current C-Core (Unified) |
| :--- | :--- | :--- |
| **Algorithm** | Multi-pass Global | Single-pass Sequential Greedy |
| **Priority** | Absolute highest magnitude wins | Latest highest magnitude wins |
| **Context** | Full signal required (Batch only) | Local window (Streaming friendly) |
| **Parity** | Diverged from real-time Max | Identical across Max and Python |

## 4. Why the C-Core Logic was Prioritized

The primary goal of the current architecture is **perfect synchronization between real-time performance and offline visualization**.

- **The Trade-off**: While SciPy's global sorting provides a "mathematically optimal" set of peaks for an entire file, it is impossible to implement in a low-latency real-time environment like Max MSP.
- **The Solution**: By using the C-core's sequential greedy logic in the Python visualizer (via the Cython extension), we ensure that the graph exactly matches what the Max object "hears," even if the peak selection differs slightly from a global SciPy pass in extremely dense areas.

## 5. Technical Speculation: Global Refinement

While the current system priorities parity, future work could include a "Global Refinement" pass for offline analysis:

1.  The Python script could perform an initial C-core pass to get baseline envelopes.
2.  A second pass could use global optimization to refine the peak list for purely visual purposes.
3.  **Note**: This would break bit-perfect parity with the real-time engine and is currently not planned.

## 6. Conclusion

The transition from SciPy-based peak detection to the unified C-core greedy logic marks a shift from "mathematical optimality per file" to **"operational parity across environments."** The current implementation ensures that `analyze_files.py` serves as a deterministic "visual proof" of the logic running inside the `analyze~` Max external.

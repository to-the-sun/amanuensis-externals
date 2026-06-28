# Peak Distance Implementation Comparison: C vs. Python

This report details the differences in the **Minimum Peak Distance** logic between the high-performance C engine (`cumulative_transience.c`) and the original Python reference (from another repository). 

## 1. Core Logic & Implementation Method
The primary difference lies in the algorithmic approach to identifying and filtering peaks:

*   **Python (Reference)**: Utilizes `scipy.signal.find_peaks(..., distance=200, prominence=0.5)`. This is a **multi-pass, global optimization** algorithm. It first identifies all local maxima, then applies prominence filtering, and finally enforces the distance constraint by processing peaks in order of their magnitude.
*   **C (Engine)**: Implements a **single-pass, greedy linear scan**. It processes frames sequentially (0 to N) and makes immediate decisions about whether to keep or replace a peak based on its temporal proximity to the previously accepted peak.

## 2. Selection Strategy: "Knock-out" Logic
The two approaches handle "peak competition" (multiple peaks within a 200ms window) differently:

*   **Python (Global Sorting)**: SciPy sorts all candidate peaks by magnitude first. It keeps the absolute highest peak in the signal and invalidates any neighbors within 200ms. It then repeats this process for the next highest remaining peak. This ensures that the **mathematically optimal** set of highest peaks is preserved across the entire timeline.
*   **C (Sequential Replacement)**: The C engine uses a greedy "replace if higher" logic.
    - If a new peak (B) is found within 200ms of the previous peak (A), and B is higher than A, B **replaces** A.
    - However, because it is sequential, if B replaces A, but a third peak (C) later arrives and is even higher than B (but still within 200ms of where A was), the algorithm only ever "remembers" the most recent peak.
    - This can lead to different results in dense transient areas compared to SciPy's global view.

## 3. Interaction with Prominence
Prominence filtering, which ensures a peak stands out from its local background, is handled differently:

*   **Python**: The `prominence=0.5` check is applied to **all** candidate peaks *before* the distance filter. Every peak in the final output is guaranteed to meet the 0.5 threshold.
*   **C**: The prominence check is only explicitly performed when a peak is "new" (i.e., not within 200ms of the previous one). If a peak is a **replacement** for a previous peak within the 200ms window, the C code currently assumes it is valid if it is higher than the peak it is replacing, potentially bypassing a fresh topological prominence check for the replacement peak.

## 4. Technical Constants & Resolution
Despite the logic differences, the fundamental constants are aligned for parity:

*   **Temporal Resolution**: Both operate on **1ms frames**.
    - Python: `hop_length = int(sr * 0.001)`
    - C: `hop_length = sr / 1000`
*   **Distance Threshold**: Both use a **200ms window**.
    - Python: `distance=200`
    - C: `f - temp_peaks[peak_count-1] < 200` (where `f` is the current frame index).

## 5. Summary Comparison

| Feature | Python (Original) | C (Current) | Impact |
| :--- | :--- | :--- | :--- |
| **Algorithm** | Multi-pass (SciPy) | Single-pass (Custom) | Performance vs. Precision trade-off |
| **Strategy** | Global Priority (Highest wins) | Sequential Greedy (Latest/Highest wins) | Slight differences in peak timing in dense areas |
| **Prominence** | Strict Pre-filter | Conditional (Bypassed on replacement) | C might accept slightly "flatter" replacements |
| **Resolution** | 1ms | 1ms | Identical |
| **Distance** | 200ms | 200ms | Identical |

## 6. Relative Benefits & Trade-offs

### C (Single-Pass Greedy)
*   **Pros**:
    - **Performance**: Constant memory overhead and O(N) complexity.
    - **Streaming Friendly**: Ideal for real-time DSP where the full signal is not known in advance.
    - **Simplicity**: No complex sorting or multi-pass logic required.
*   **Cons**:
    - **Sub-optimal Selection**: In dense transient clusters, it might pick a slightly lower peak if it appears later in the sequential scan, provided it is still within the "competition" window of a suppressed higher peak.

### Python/SciPy (Multi-Pass Global)
*   **Pros**:
    - **Optimality**: Guarantees the absolute highest energy peaks are preserved by prioritizing them first.
    - **Consistency**: The result is deterministic and independent of scan direction.
*   **Cons**:
    - **Resource Intensive**: Requires the entire signal to be buffered, leading to higher memory usage and O(N log N) complexity due to sorting.
    - **Batch Only**: Cannot be effectively implemented in a true low-latency streaming environment.

## 7. Strategy Evaluation: Which is Better?

*   **Better for Real-time (C)**: The sequential approach is significantly better for the `analyze~` Max object. Since the object processes audio in small hops (100ms), it needs to make immediate decisions. A global sorting strategy would require buffering the entire track or introducing massive look-ahead latency.
*   **Better for Offline (Python)**: The SciPy approach is superior for `analyze_files.py` and the associated video generation. In this context, processing time is secondary to achieving a "mathematically perfect" ground-truth visualization of spectral transients.

## 8. Technical Speculation: Replicating SciPy in C

To switch the C engine to exactly replicate the SciPy strategy, the following architectural changes would be necessary:

1.  **Candidate Buffering**: Modify `analyzer_analyze_audio` to store all initial peak candidates (those passing the rolling threshold and prominence checks) into a temporary structure before enforcing any distance constraints.
2.  **Magnitude Sorting**: Implement a sorting algorithm (e.g., `qsort` with a custom comparator) to order these candidate peaks by their flux magnitude in descending order.
3.  **Iterative Invalidation Pass**:
    - Create a boolean mask or flag array for all candidates.
    - Iterate through the sorted list.
    - For each candidate, if it hasn't been "invalidated," mark it as accepted and then scan all other candidates to invalidate any that fall within a 200ms radius.
4.  **Temporal Re-sorting**: After filtering, the remaining accepted peaks must be re-sorted by their original frame index (time) before resonance scoring can proceed.
5.  **Impact on Max Object**: This would effectively turn `analyze~` into an offline-only analyzer, or require a significant "analysis delay" (likely several seconds) to allow the global logic to resolve peaks before they can be output.

## Conclusion
The C implementation is designed for streaming performance and avoids the memory/computational overhead of sorting all peaks globally. While it effectively replicates the *intent* of the 200ms minimum distance, users may notice subtle differences in peak selection in complex audio where multiple high-energy transients occur in very close proximity.

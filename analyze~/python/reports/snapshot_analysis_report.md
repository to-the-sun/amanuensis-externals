# Cumulative Transient Analysis: Snapshot & Resonance Score Logic

This report details the technical operations that occur when a "snapshot" is taken for calculating the resonance score of a primary peak. It distinguishes between high-level orchestration (**Python Layer**) and high-performance numerical DSP (**C Core**).

## 1. Orchestration and Windowing

### A. Temporal Orchestration (**Python Layer**)
The process is initiated within the `update` function of the video generator. While the video renders at 30 FPS (~33ms per frame), the underlying analysis in the C core catchs up at a **1ms resolution**.
*   **Sub-Frame Accuracy**: For every millisecond between video frames, the Python script (`analyze_files.py`) processes any new peaks finalized by the C core.
*   **39ms Visual Smoothing**: At each video frame, the Python layer prunes the score pool to a 39ms window. The displayed "Score" is the average of all scores currently within this window.

### B. Temporal Windowing (Resonance) (**C Core**)
When a peak is processed by the C core (`analyzer_process_peak`), it extracts a **5001-sample window** from the onset strength envelope.
*   **Resolution**: 1ms per sample (representing 5000ms of history + the current 1ms at the peak).
*   **Alignment & Padding**: The window is aligned to end exactly at the peak. If the peak occurs within the first 5 seconds, the C core handles zero-padding at the beginning.

## 2. Normalization (**C Core**)

To ensure that rhythmic influence is relative, the C core normalizes each window:
*   **Normalization Factor**: Calculated as $\frac{peak\_val}{max\_peak}$.
*   **The Snapshot**: The resulting scaled array is designated as the **`snapshot`**.

## 3. Resonance Score Calculation (**C Core**)

The **Resonance Score** measures how well the rhythmic "history" in the new snapshot aligns with the collective history in the `accumulated_buffer`.

### A. Historical Context (The 99ms Offset)
The C core calculates baseline statistics (`avg`, `max_v`, and `min_v`) from the `accumulated_buffer`. Crucially, it **excludes the last 99ms of the buffer** from these calculations to prevent the primary peak from biasing its own resonance score.

### B. Rhythmic Alignment (Peak Mapping)
The C core identifies which historical moments in the `snapshot` contain relevant rhythmic energy by checking a list of all valid peak indices across all bands.

### C. The Qualifier ($Q$)
For every historical peak identified in the new snapshot (at relative index $sp_{idx}$), the C core checks the energy level ($val$) in the `accumulated_buffer` at that same offset:
*   **Reward**: If $val > avg$, then $Q = \frac{val - avg}{max\_v - avg}$.
*   **Penalty**: If $val < avg$, then $Q = \frac{val - avg}{avg - min\_v}$.

### D. Final Score Determination
The C core calculates a **`qualifier_sum`** by adding all individual qualifiers. The final **Total Score** is the product of the primary `peak_val` and this sum:
$$\text{Total Score} = peak\_val \times \sum Q$$

## 4. Accumulation and State Management (**C Core**)

*   **Buffer Update**: After score calculation, the C core adds the `snapshot` to the `accumulated_buffer`.
*   **Sliding Window Cleanup**: Each snapshot is stored in a linked list. During the `analyzer_update_metrics` call, the C core identifies snapshots older than **15 seconds** and subtracts them from the `accumulated_buffer`.

## 5. Visual Feedback and UI (**Python Layer**)

While the C core handles the numbers, the Python layer (`analyze_files.py`) handles the representation:
*   **Visual Snapshot**: Renders the current snapshot line in green on the historical buffer plot.
*   **Score Animation**: Displays floating text when a peak is detected.
*   **Snapshot Bar**: Manages the bottom bar visualization, aligning scores relative to the latest peak.

## 6. Metric Recording (**Python Layer**)

Upon completion, the Python script records the final aggregated metrics to persistent storage.
*   **Target Location**: `D:\[Library]\[Audio]\[Works]\[Projects]\{SongName}\ratings.txt`
*   **Metrics**: Rating (mean resonance), Standard Deviation, Contrast, and Bar Length Deviation (rhythmic stability).

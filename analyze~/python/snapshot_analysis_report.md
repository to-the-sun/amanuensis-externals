# Cumulative Transient Analysis: Snapshot & Resonance Logic

This report details the operations involved in calculating resonance scores, distinguishing between the Python orchestration layer and the high-performance C core.

## 1. Orchestration and Windowing

### A. Temporal Orchestration (Python Layer)
The Python layer (`analyze_files.py`) manages the high-level timeline.
- **Accuracy**: For every millisecond between video frames (30 FPS), it calls the C core to process new audio data and peaks.
- **Visual Smoothing**: Displays a "Rolling Window Snapshot" (39ms), which is the average of resonance scores for peaks currently within that window.

### B. Temporal Windowing (C Core)
When a peak is processed by the C core (`analyzer_process_peak`), it extracts a **5001-sample window** from the flux envelope.
- **Resolution**: 1ms per sample (5000ms history + current sample).
- **Snapshot Creation**: This window is normalized relative to the current `max_peak` to create the **snapshot**.

## 2. Resonance Score Calculation (C Core)

The **Resonance Score** measures how well the rhythmic history in the new snapshot aligns with the collective history in the `accumulated_buffer`.

### A. Historical Context (The 99ms Offset)
The C core calculates baseline statistics (`avg`, `max_v`, `min_v`) from the `accumulated_buffer`, excluding the last 99ms to prevent the current transient from biasing its own score.

### B. Rhythmic Alignment (Peak Mapping)
The C core uses the list of all valid peak indices to identify which historical moments in the current snapshot contain relevant rhythmic energy.

### C. The Qualifier ($Q$)
For each historical peak in the snapshot, the C core checks the energy level in the `accumulated_buffer` at that offset:
- **Reward**: If energy > average, $Q = (val - avg) / (max\_v - avg)$.
- **Penalty**: If energy < average, $Q = (val - avg) / (avg - min\_v)$.

### D. Final Score
The **Total Score** is the product of the peak's flux value and the sum of its qualifiers:
$$\text{Total Score} = peak\_val \times \sum Q$$

## 3. State Management (C Core)
- **Buffer Update**: After score calculation, the snapshot is added to the `accumulated_buffer`.
- **Sliding Window Cleanup**: Snapshots older than **15 seconds** are subtracted from the `accumulated_buffer` to ensure rhythmic memory remains localized.

## 4. Visual Representation (Python Layer)
- **Visual Feedback**: Renders flashes and score animations in the video reports.
- **Metric Recording**: Final aggregated metrics (Rating, Std Dev, Contrast, Bar Length Deviation) are recorded to the project's `ratings.txt` file upon completion.

## 5. Conclusion
The division of labor ensures that heavy numerical processing occurs in the C core while the Python layer provides high-fidelity visualization and orchestration, resulting in a synchronized and accurate analysis system.

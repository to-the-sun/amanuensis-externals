# Spectral Analysis Pipeline Report: Data Flow and Orchestration

This report clarifies the architectural relationship between the core C transience algorithm (`cumulative_transience.c`) and its host environments (Max and Python), specifically addressing the flow of spectral data and peak detection.

## 1. Where does the Spectral Analysis occur?

**The spectral analysis (FFT, Mel-filtering, and Spectral Flux calculation) occurs entirely within the C core.**

Specifically, the function `analyzer_analyze_audio` in `cumulative_transience.c` handles the following:
1.  **Windowing & FFT**: Converts raw time-domain audio into the frequency domain.
2.  **Mel-Filtering**: Aggregates FFT bins into 128 Mel-spaced bands.
3.  **Spectral Flux**: Calculates the rate of change in energy for each band.
4.  **Band Summation**: Groups the 128 Mel bands into the 4 analysis bands (0-31, 32-63, 64-95, 96-127).
5.  **Peak Detection**: Applies a rolling threshold and prominence check to the flux envelopes to identify transient candidates.

The results are stored in a `FullAnalysisResult` structure, which contains the raw flux envelopes and a list of detected peak indices for each band.

## 2. Why is there a "Back and Forth" between C and the Host?

The "orchestration" pattern currently exists because of the difference between **Detection** and **Resonance Analysis**:

*   **Detection (Stateless/Local)**: Identifying a peak requires looking at a short neighborhood of audio (e.g., 200ms). This is handled by `analyzer_analyze_audio`.
*   **Resonance (Stateful/Global)**: The resonance score of a peak depends on the *history* of all previous peaks across all bands over a 5-second window. This is handled by `analyzer_process_peak`.

### The Rationale for Host Orchestration
By handing the detected peaks back to the host (Max or Python), the system currently manages specific environmental needs:
1.  **Real-Time Windowing (Max)**: The host tracks which peaks have already been processed across sliding 100ms cycles to avoid double-counting.
2.  **Timeline Synchronization**: The host maps "local" indices to "global" timestamps.

---

## 3. Achieved Evolution: Host-Side C-Core Orchestration

The system has been evolved to eliminate the "back-and-forth" entirely. In this model, the host hands the C core a chunk of audio (15.2 seconds), and the C core returns a list of fully analyzed `PeakResult` objects.

### Implementation of Inter-band Synchronization
The C core now handles the orchestration:
1.  **Unified Windowing**: Performs FFT and Flux analysis for all bands across the 15.2s window.
2.  **Global Peak Sorting**: Extracts all peaks across all bands and performs a **global chronological sort**.
3.  **Stateful Processing**: Feeds sorted peaks into the resonance engine in the exact order they occurred.
4.  **Active-Zone Filtering**: Only returns peaks and metrics from the "active" 100ms zone of the window.

**Result**: This completely eliminates the `Inter-band Processing Order` discrepancy. Both Max and Python now process peaks in strict chronological order across all bands.

---

## 4. Impact Analysis (15.2s Unified Windowing)

The Python visualizer and the Max external are now perfectly synchronized through the 15.2s sliding window model.

### Impact on Documented Discrepancies

#### 1. Global vs. Converging `max_peak`
- **Upside**: High. Both pipelines would converge on the same "local" maximum.
- **Downside**: Early transients might still be slightly different if the "start" of the 15s window differs between Max's stream and Python's batch pass.
- **Difficulty**: Medium. Requires implementing a sliding-window max calculation in the C core.

#### 2. Peak Suppression and Minimum Distance
- **Upside**: Elimination of "double-hits" in Max. A 200ms lookahead allows the C core to see if a larger peak is coming and suppress the current one accordingly.
- **Downside**: Introduces 200ms of latency to the final peak score delivery (acceptable in the current `async_worker` model).
- **Difficulty**: Low. The lookahead logic is straightforward once the window is large enough.

#### 3. Inter-band Processing Order
- **Upside**: Absolute synchronization. If the C core handles the chronological sort, the state of the "resonance buffer" will be identical in both environments.
- **Downside**: Slightly higher memory usage in the C core to store and sort peak structures before processing.
- **Difficulty**: Medium. Requires moving host-side sorting logic into the `cumulative_transience.c` library.

#### 4. Resonance Context (`all_valid_peak_indices`)
- **Upside**: Both pipelines would see the exact same 15s history, eliminating the "truncated context" issue where Max misses a peak just outside its 6s window.
- **Downside**: Processing 15s of audio is more CPU-intensive than 6s (~250% increase).
- **Difficulty**: Low. Already implemented for Max (currently using 6s, can be bumped to 15s).

#### 5. Mel Spectrogram Normalization (Spectral Breathing)
- **Upside**: Predictable behavior. The "breathing" would be identical in Python and Max.
- **Downside**: 15s is still a finite window; a very loud sound entering the window will still "duck" the flux of other bands.
- **Difficulty**: None. This is a natural result of unified windowing.

#### 6. Temporal Resolution
- **Upside**: Ensuring both use the same hop-size logic regardless of system SR. Max typically operates at a standard 44.1kHz, so they should generally match.
- **Downside**: If a discrepancy occurs, Python must resample audio to match the Max environment's rate before analysis.
- **Difficulty**: Low. `librosa` handles resampling easily.

#### 7. STFT Padding and Window Edges
- **Upside**: By using a 15s window + 200ms lookahead/lookbehind, the "active" analysis zone is kept away from the tapered edges of the STFT, providing "clean" data to both pipelines.
- **Downside**: Slightly more audio data must be processed than is strictly necessary for the output frames.
- **Difficulty**: Low. Standard overlapping window technique.

## 5. Implementation Speculation: Frequency and Manner

If the unified 15.2s windowing model were implemented, the analysis would be called in the following manner:

- **Frequency**: Both pipelines would operate on a **100ms periodic cycle**.
    - In **Max**, the background thread would trigger every 4,410 samples (at 44.1kHz).
    - In **Python**, the orchestration loop would step through the file in increments of 0.1 seconds, slicing the necessary audio chunk for each step.
- **Manner**: The Host would hand a raw pointer to a contiguous block of 15.2 seconds of PCM audio (approximately 670,320 samples at 44.1kHz) to the C core. 
    - The C core would return a list of `PeakResult` objects found within the "active" 100ms zone of that window.
    - The Host would then be responsible for only one thing: **outputting or recording these results.**

### Algorithmic Efficiency and Sample Utilization

One might worry that handing 15.2 seconds of audio every 100ms is inefficient. However, the C core can be designed to use only the samples necessary for each step:

1.  **Incremental FFT/Flux**: The C core doesn't need to re-calculate the STFT for the entire 15s every 100ms. It can maintain a small internal spectral cache, calculating only the *new* 100ms of spectral flux and appending it to its internal history.
2.  **Targeted Peak Detection**: Peak detection is only run on the "active" 100ms zone (plus the 200ms lookahead). The previous 15s of flux are held in memory as context, but are not re-scanned for peaks.
3.  **Contextual Resonance**: The resonance calculation only looks back 5s into its existing cumulative energy buffer. 

By using this **Incremental Processing Model**, the CPU cost per 100ms cycle remains nearly identical to the current 6s model, regardless of the fact that the C core has access to a larger 15.2s "window" of raw audio for initial state stabilization.

## 6. Conclusion
The "back-and-forth" was an artifact of early development. Moving to a **Unified 15.2s C-Managed Window** (15s context + 200ms lookahead) called every 100ms is the most robust way to ensure that the Python visualizer accurately reflects the Max external's real-time performance.

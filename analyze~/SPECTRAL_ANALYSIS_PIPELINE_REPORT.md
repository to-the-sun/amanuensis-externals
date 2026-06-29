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

## 3. Proposed Evolution: Full C-Core Orchestration

The system could be evolved to eliminate the "back-and-forth" entirely. In this model, the host would simply hand the C core a chunk of audio (e.g., 15.2 seconds), and the C core would return a list of fully analyzed `PeakResult` objects.

### Implementation of Inter-band Synchronization
To achieve this, the C core would need to:
1.  Perform the FFT and Flux analysis for all bands for the entire window.
2.  Extract all peaks across all bands.
3.  **Perform a global chronological sort** of every peak regardless of its band.
4.  Feed these peaks into the stateful resonance engine in the exact order they occurred.

**Upside**: This would completely eliminate the `Inter-band Processing Order` discrepancy. Max would no longer process Band 0 then Band 1; it would process "the first peak in the window," which might be in Band 3.

---

## 4. Speculative Impact Analysis (15s Unified Windowing)

The primary goal is to synchronize the Python visualizer with the Max external. Since the Max external is the "source of truth" for the real-time algorithm, the Python offline analysis should be modified to mimic the 15s sliding window approach.

Furthermore, from a psychoacoustic perspective, a 15-second "rolling" `max_peak` is more realistic than a global one, as human listeners adapt to the current loudness and transient density of a song rather than "knowing" the loudest moment 3 minutes in the future.

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
- **Upside**: Ensuring both use the same hop-size logic regardless of system SR.
- **Downside**: Requires Python to resample audio to the "standard" rate expected by the Max environment (e.g., 48kHz) before analysis.
- **Difficulty**: Low. `librosa` handles resampling easily.

#### 7. STFT Padding and Window Edges
- **Upside**: By using a 15s window + 200ms lookahead/lookbehind, the "active" analysis zone is kept away from the tapered edges of the STFT, providing "clean" data to both pipelines.
- **Downside**: Slightly more audio data must be processed than is strictly necessary for the output frames.
- **Difficulty**: Low. Standard overlapping window technique.

## 5. Conclusion
The "back-and-forth" was an artifact of early development. Moving to a **Unified 15.2s C-Managed Window** (15s context + 200ms lookahead) is the most robust way to ensure that the Python visualizer accurately reflects the Max external's real-time performance.

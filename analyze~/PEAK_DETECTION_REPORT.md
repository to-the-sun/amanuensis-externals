# Peak Detection Report: Analysis and Remediation

This report details the current peak detection mechanism in the `cumulative_transience` core and addresses the issue of over-detection observed after the transition to the **Unified 15.2s Windowing Model**.

## 1. How Peak Detection Works Right Now

The peak detection logic is encapsulated within `analyzer_analyze_audio` (and replicated in the new `analyzer_analyze_chunk`). It operates on the **Spectral Flux** (Onset Strength) envelopes for each of the 4 frequency bands.

### Units and Calculation
The Onset Strength is calculated in terms of **average positive change in decibels (dB) per frame**.
1.  The Mel spectrogram is converted to log-power (dB).
2.  The difference between the current frame and the previous frame is calculated for every Mel-band.
3.  Only positive differences (increases in energy) are accumulated.
4.  The final Onset Strength for a band is the **average of these positive dB changes** across all Mel-bins assigned to that band (32 bins per band).

### A. Candidate Identification
A frame `f` is considered a peak candidate if it meets three local criteria:
1.  **Local Maximum**: `env[f] > env[f-1]` and `env[f] > env[f+1]`.
2.  **Above Threshold**: `env[f] > thresh[f]`. The threshold is a **15-second rolling average** of the flux envelope.
3.  **Minimum Distance**: A new peak must be at least **200ms** (200 frames) away from the previous peak in the same band. If a larger peak is found within the 200ms window, it replaces the smaller one.

### B. Prominence Check
Currently, a very basic prominence check is applied:
-   `prom = env[f] - max(left_min, right_min)`
-   `left_min` / `right_min`: These represent the "valley" floors on either side of the peak. They are calculated by searching outward from the candidate frame `f` until a value higher than `env[f]` is encountered or the buffer edge is reached. The lowest flux value found during these searches is the local minimum for that side.
-   `if (prom >= 0.5f)`: The peak is accepted.

## 2. Why Over-detection is Occurring

The implementation of the **Unified 15.2s Windowing Model** has introduced several side effects that impact the sensitivity of this logic:

### A. Stable Threshold Convergence
In the previous 6s batch model, the rolling threshold was often "truncated" or volatile due to the shorter window. With a full 15.2s context, the rolling average threshold is much more stable and accurately represents the local noise floor. While this is mathematically superior, it means that even tiny fluctuations in the flux envelope that stay just above the stable noise floor are now being identified as valid transients.

### B. Low Prominence Floor
The hardcoded prominence floor of `0.5` flux units is extremely lenient. In clean, high-dynamic-range audio, the flux at a true transient can reach values of 20-50. A fluctuation of 0.5 is effectively background noise or "spectral jitter" that would have been ignored in the previous volatile model.

### C. Interaction with "Spectral Breathing"
Although the 15.2s window is more stable, the spectrogram is still normalized relative to the loudest frame in that window. In quiet sections of a song, this normalization "boosts" the flux values of low-level noise. Combined with the stable threshold and low prominence floor, the system begins detecting peaks in the silence.

## 3. Role of Average Band Energy

The current logic uses the **rolling average flux** as the detection threshold.
-   **The Intent**: This is an adaptive "Constant False Alarm Rate" (CFAR) detector. It adjusts its sensitivity based on the current density and intensity of the music.
-   **The Problem**: Flux average is not the same as spectral energy. A band with high static energy (e.g., a constant synth pad) has low flux. Consequently, its threshold stays low, and any small rhythmic fluctuation on top of that loud pad triggers a peak.

## 4. Proposed Alleviations (Restoring Performance)

To return the peak detection to its original, more selective behavior, the following remediations are proposed:

### Strategy 1: Increase Prominence and Absolute Threshold
We should revert to more conservative constants. A true transient should be significantly higher than its neighborhood.
-   **Action**: Increase prominence floor from `0.5` to `2.0` (or make it a percentage of `max_peak`).
-   **Action**: Implement an **absolute flux floor** (e.g., `1.0`). If the flux is below this value, it's noise, regardless of the rolling threshold.

### Strategy 2: Adaptive Prominence
The "local threshold" (`thresh[f]`) used here is the **15-second rolling average of the spectral flux**. It represents the expected "activity level" in that frequency band over a long context window.
Instead of a fixed `0.5`, the prominence should scale with the local threshold.
-   **Action**: `if (prom >= thresh[f] * 0.5)`: This ensures that in loud sections, we require a large spike to trigger a peak, while in quiet sections, we are more sensitive (but still limited by the absolute floor).

### Strategy 3: Global Magnitude Gating (Secondary Peak-Average Gating)

This strategy uses the **Average Magnitude of Detected Peaks** within the 15.2s window as a dynamic filter to eliminate low-level "jitter" and phantom transients.

#### Implementation Details:
1.  **Stage 1 Detection**: The system identifies all candidate peaks using the standard criteria (local maximum, 15s rolling flux average, distance, and prominence).
2.  **Peak Magnitude Averaging**: The system calculates the average **Spectral Flux** (Onset Strength) value of every peak detected in the 15.2s window.
3.  **Stage 2 Gating**: A candidate peak is only committed to the stateful resonance engine if its Onset Strength is greater than or equal to this **peak-average**.
4.  **Visual Representation**: The horizontal threshold lines in the 4-band transient graph now represent this **dynamic peak average** instead of the underlying 15s rolling flux average.

#### Speculation on Efficacy:
-   **Upside**: High selectivity. By averaging only the peaks themselves, the filter ignores the vast majority of the signal (the quiet gaps) and focuses only on the distribution of "hits". This ensures that in a dense rhythmic section, only the primary hits are kept, while in a sparse section, the average drops and allows more subtle transients to pass.
-   **Upside**: Intuitive Visualization. The user sees the threshold line jump or settle based on the "average strength" of the current rhythm.
-   **Downside**: If a section has one massive transient and many medium ones, the average might be skewed high, causing the medium transients to be gated despite being rhythmically valid.

## 5. Summary of Technical Definitions
-   `left_min` / `right_min`: The lowest flux values encountered when searching outward from a peak candidate until a higher value is found.
-   `thresh[f]`: The 15-second rolling average of the spectral flux (used in the primary adaptive thresholding).
-   `max_db`: The peak decibel level found within the 15.2s window, used for STFT normalization.

## 6. Conclusion

The transition to full orchestration and stable windowing has revealed a "leniency bug" in the core detection constants. By increasing the prominence floor and implementing an absolute flux floor, we can restore the selective and accurate peak detection that the system exhibited before the session began.

# Peak Detection Report: Analysis and Remediation

This report details the peak detection mechanism in the `cumulative_transience` core and addresses the issues that were resolved during the transition to the **Unified 15.2s Windowing Model**.

## 1. Current Peak Detection Mechanism

The peak detection logic is encapsulated within `analyzer_analyze_chunk` (and mirrored in the batch analysis). It operates on the **Spectral Flux** (Onset Strength) envelopes for each of the 4 frequency bands.

### Units and Calculation
The Onset Strength is calculated in terms of **average positive change in decibels (dB) per frame**.
1.  The Mel spectrogram is converted to log-power (dB).
2.  The difference between the current frame and the previous frame is calculated for every Mel-band.
3.  Only positive differences (increases in energy) are accumulated.
4.  The final Onset Strength for a band is the **average of these positive dB changes** across all Mel-bins assigned to that band (32 bins per band).

### A. Candidate Identification
A frame `f` is considered a peak candidate if it meets four local criteria:
1.  **Local Maximum**: `env[f] > env[f-1]` and `env[f] > env[f+1]`.
2.  **Above Threshold**: `env[f] > thresh[f]`. The threshold is the **dynamic historical peak-based rolling midpoint**.
3.  **Absolute Floor**: `env[f] >= 0.0`. (Note: This is currently set to 0.0, effectively allowing all positive flux).
4.  **Minimum Distance**: A new peak must be at least **200ms** (200 frames) away from the previous peak in the same band. If a larger peak is found within the 200ms window, it replaces the smaller one.

### B. Prominence Check
An adaptive midpoint-based prominence check is applied:
-   `prom = env[f] - max(left_min, right_min)`
-   `left_min` / `right_min`: These represent the "valley" floors on either side of the peak.
-   **Decision**: `if (prom > band_midpoint)`: The peak is accepted.

## 2. Historical Over-detection Issues

During the implementation of the **Unified 15.2s Windowing Model**, several side effects impacted detection sensitivity:

### A. Stable Threshold Convergence
With a full 15.2s context, the rolling threshold became much more stable compared to the previous 6s volatile model. While mathematically superior, it meant that even tiny fluctuations above the stable noise floor were being identified as valid transients.

### B. Low Prominence Floor (Historical)
Previously, a hardcoded prominence floor of `0.5` flux units was used. In clean audio, a fluctuation of 0.5 is effectively background noise or "spectral jitter."

### C. Interaction with "Spectral Breathing"
The spectrogram is normalized relative to the loudest frame in the 15.2s window. In quiet sections, this normalization "boosts" low-level noise. Without adaptive prominence, the system detected peaks in the silence.

## 3. The Midpoint Strategy (Currently Implemented)

The system now utilizes an adaptive midpoint-based approach to replace the historical rolling average thresholds and fixed prominence floors.

### Implementation Details:
1.  **Midpoint Thresholding**: Both the primary detection threshold and the prominence threshold use a **dynamic historical peak-based rolling midpoint**.
2.  **Dynamic Sub-Window**: The midpoint is calculated from a sub-window at the end of the 15s flux cache. The size of this sub-window (`midpoint_lookback`) is dynamic per frequency band, calculated as `15000.0 - (previous_lookback_ms / quantity_in_previous_lookback)`.
3.  **Adaptive Prominence**: A peak is only valid if its prominence is **greater than the rolling midpoint** of the band's flux (`prom > midpoint`). This ensures that a peak must stand out significantly relative to the typical activity level of that band.
4.  **Visual Representation**: In the visualizer, horizontal threshold lines show this **dynamic historical peak-based rolling flux midpoint**.

## 4. Resolved Historical Challenges

The following challenges were identified and addressed during development:

**1. The Barrage of Tiny Peaks (Low-level Jitter)**
-   **Status**: **RESOLVED**.
-   **Historical Cause**: Spectrogram normalization without sufficient prominence constraints caused the system to detect tiny fluctuations in noise.
-   **Resolution**: The adaptive prominence check (`prom > midpoint`) ensures that a spike must be significant relative to the band's activity, providing a natural filter against jitter.

**2. Missing Large Peaks (Threshold Inflation)**
-   **Status**: **RESOLVED**.
-   **Historical Cause**: Processing thousands of tiny noise-peaks inflated the rolling average flux, pushing the threshold too high for valid transients.
-   **Resolution**: By rejecting noise via the adaptive prominence check *before* it affects resonance or further processing, the system maintains accurate thresholds.

## 5. Summary of Technical Definitions
-   `left_min` / `right_min`: The lowest flux values encountered when searching outward from a peak candidate until a higher value is found.
-   `thresh[f]`: The dynamic historical peak-based rolling midpoint of the spectral flux.
-   `max_db`: The peak decibel level found within the 15.2s window, used for STFT normalization.

## 6. Conclusion

The transition to full orchestration and stable windowing revealed a "leniency bug" in the historical detection constants. The current implementation of **Dynamic Midpoint Thresholds and Adaptive Prominence** has restored selective and accurate peak detection across both real-time and offline environments.

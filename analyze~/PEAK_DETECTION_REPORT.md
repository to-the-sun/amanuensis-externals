# Peak Detection Report: Analysis and Remediation

This report details the peak detection mechanism in the `cumulative_transience` core and addresses the issues that were resolved during the transition to the **Unified 15.2s Windowing Model**.

## 1. Current Peak Detection Mechanism

The peak detection logic is encapsulated within `analyzer_analyze_chunk` (and mirrored in the batch analysis). It operates on the **Spectral Flux** (Onset Strength) envelopes for each of the 4 frequency bands, but utilizes a secondary **Dynamically Smoothed Flux** signal for final validation.

### Units and Calculation
The Onset Strength is calculated in terms of **average positive change in decibels (dB) per frame**.
Additionally, a `dynamic_smoothing` parameter is derived from the flux: if the current flux is greater than the previous smoothing value, it snaps to the current flux; otherwise, it decays by **1/200th** of the distance to the current flux level each frame.
Per-frame **prominence** values are also calculated, representing the height of the current **smoothed flux** above the higher of the two nearest local minima in the smoothed signal.

1.  The Mel spectrogram is converted to log-power (dB).
2.  The difference between the current frame and the previous frame is calculated for every Mel-band.
3.  Only positive differences (increases in energy) are accumulated.
4.  The final Onset Strength for a band is the **average of these positive dB changes** across all Mel-bins assigned to that band (32 bins per band).

### A. Candidate Identification
A frame `f` is considered a peak candidate if it meets four local criteria:
1.  **Local Maximum**: `env[f] > env[f-1]` and `env[f] > env[f+1]` in the raw flux.
2.  **Above Threshold**: `env[f] > thresh[f]`. The threshold is currently the **15-second running average of the smoothed flux**.
3.  **Absolute Floor**: `env[f] >= 0.0`.
4.  **Minimum Distance**: A new peak must be at least **200ms** (200 frames) away from the previous peak in the same band. If a larger peak is found within the 200ms window, it replaces the smaller one.

### B. Prominence Check (Refined)
An adaptive prominence check is applied to the **smoothed flux** signal:
-   `prom_s = smooth_env[f] - max(left_min_s, right_min_s)`
-   `left_min_s` / `right_min_s`: The "valley" floors on either side of the peak in the smoothed signal.
-   **Decision**: `if (prom_s > smoothed_flux_avg)`: The peak is accepted.
-   `smoothed_flux_avg`: The 15-second running average of the `dynamic_smoothing` parameter.

## 2. Historical Over-detection Issues

During the implementation of the **Unified 15.2s Windowing Model**, several side effects impacted detection sensitivity:

### A. Stable Threshold Convergence
With a full 15.2s context, rolling thresholds became much more stable compared to previous volatile models. While mathematically superior, it meant that even tiny fluctuations above the stable noise floor were being identified as valid transients.

### B. Spectral Jitter
The raw flux signal often contains high-frequency "jitter" that creates false peak candidates.

## 3. The Dynamic Smoothing & Prominence Strategy (Currently Implemented)

The system now utilizes a layered approach using a dynamically smoothed flux signal to filter out jitter while maintaining responsiveness.

### Implementation Details:
1.  **Dynamic Smoothing**: Performed at 1ms resolution. If the current `flux` is greater than the previous `dynamic_smoothing` value, it snaps to match it. If the `flux` is less, it moves 1/200th of the distance toward the current `flux`.
2.  **Smoothed Flux Prominence**: Per-frame prominence is calculated from the smoothed flux signal.
3.  **Running Averages**: 15-second running averages are maintained for both `dynamic_smoothing` and `prominence`.
4.  **Final Peak Detection Logic**: A peak is triggered if the prominence of the smoothed flux signal exceeds that band's 15-second running average of smoothed flux.
5.  **Visual Representation**:
    *   **Smoothed Flux Lines**: Solid lines in band colors (trailing playhead).
    *   **Prominence Lines**: Solid lines in red shades (trailing playhead, top layer).
    *   **Horizontal Threshold Lines**: Band-colored solid lines representing the 15-second running average of smoothed flux.

## 4. Resolved Historical Challenges

The following challenges were identified and addressed during development:

**1. The Barrage of Tiny Peaks (Low-level Jitter)**
-   **Status**: **RESOLVED**.
-   **Resolution**: The use of `dynamic_smoothing` filters out instantaneous jitter, while the prominence check against the 15s average ensures that a transient must be significant relative to the long-term activity level.

**2. Missing Large Peaks (Threshold Inflation)**
-   **Status**: **RESOLVED**.
-   **Resolution**: By basing the threshold on the 15s average of *smoothed* flux rather than raw peaks, the system avoids the "runaway threshold" problem caused by noise-floor spikes.

## 5. Summary of Technical Definitions
-   `left_min_s` / `right_min_s`: The lowest smoothed flux values encountered when searching outward from a candidate until a higher value is found.
-   `smoothed_flux_avg`: The 15-second running average of the `dynamic_smoothing` parameter.

## 6. Conclusion

The implementation of **Dynamic Smoothing and Smoothed Prominence Thresholding** has provided a robust solution to the over-detection issues encountered with raw flux signals, ensuring selective and musically relevant peak detection.

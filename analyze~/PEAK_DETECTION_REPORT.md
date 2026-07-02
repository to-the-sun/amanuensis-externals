# Peak Detection Report: Adaptive Midpoint Model

This report analyzes the evolution and current state of the peak detection algorithm within the `cumulative_transience` system.

## 1. Historical Problem: Oversensitivity and Jitter

Early versions of the incremental analyzer suffered from "leniency bugs" where the system detected too many low-level peaks (jitter) in quiet sections, while occasionally missing large transients in dense sections due to threshold inflation.

## 2. Role of Average Band Energy (Past Context)

Previously, the logic used a simple **rolling average flux** as the detection threshold.
- **The Problem**: Flux average is not the same as spectral energy. A band with high static energy (e.g., a constant synth pad) has low flux. Consequently, its threshold stayed low, and small rhythmic fluctuations on top of loud pads triggered false peaks.

## 3. Current Implementation: Adaptive Midpoint Model

The system now utilizes an adaptive midpoint-based approach to ensure selective and accurate peak detection across all dynamic ranges.

### Strategy: Midpoint Thresholds and Adaptive Prominence

1.  **Detection Check (Noise Floor)**: The absolute flux floor is currently set to **0.0 dB**, effectively disabling it to allow the adaptive logic to handle sensitivity.
2.  **Midpoint Thresholding**: Both the primary detection threshold and the prominence threshold use a **999-millisecond rolling midpoint** (calculated from the end of the 15s flux cache) for each band. Midpoints track the local dynamic range of the flux and provide a balanced baseline.
3.  **Adaptive Prominence**: A peak is only valid if its prominence is **greater than the rolling midpoint** of the band's flux (`prom > midpoint`). This ensures that a peak must stand out significantly relative to the typical activity level of that frequency band.
4.  **Visual Representation**: In the visualizer, horizontal threshold lines display this **999-millisecond rolling flux midpoint**.

## 4. Resolved Implementation Challenges

### Low-level Jitter (The "Barrage of Tiny Peaks")
- **Status**: **RESOLVED**.
- **Challenge**: In the incremental model, preserving low-level noise could lead to a barrage of tiny noise-peaks.
- **Resolution**: The adaptive prominence check (`prom > midpoint`) ensures that even at low levels, a spike must be significant relative to the band's midpoint activity to be counted, providing a natural filter against jitter without requiring a hard absolute floor.

### Missing Large Peaks (Threshold Inflation)
- **Status**: **RESOLVED**.
- **Challenge**: When noise-peaks were processed, they contributed to the 15-second rolling average, inflating the threshold and causing large, valid peaks to be missed.
- **Resolution**: By rejecting noise via the prominence check *before* it affects long-term metrics, valid major transients stand out more clearly against a more accurate, lower noise floor.

## 5. Summary of Technical Definitions
- `left_min` / `right_min`: The lowest flux values encountered when searching outward from a peak candidate until a higher value is found.
- `thresh[f]`: The 999-millisecond rolling midpoint of the spectral flux (used in the primary adaptive thresholding).
- `max_db`: The peak decibel level found within the 15.2s window, used for STFT normalization.

## 6. Conclusion

The transition to full orchestration and stable windowing has been solidified by the Adaptive Midpoint Model. By utilizing local midpoints for both primary thresholding and prominence checks, the system achieves the selective and accurate peak detection required for both real-time performance and high-fidelity visualization.

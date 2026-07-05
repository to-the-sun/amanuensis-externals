# Technical Report: Prominence Decorrelation in `analyze~` Visualizer

## Overview
A decorrelation was observed between the numerical prominence values displayed in the debug console of `analyze_files.py` and the heights of the corresponding red-shaded prominence envelopes on the graph. This report details the investigation into this discrepancy and its resolution.

## Core Discrepancy: Windowing and Latency
The primary cause of the decorrelation was a disparity in the temporal window and logic used for prominence calculation during peak detection versus the envelope generation used for visualization.

### 1. Peak Detection Prominence (Historical)
Previously, prominence for peak detection was calculated within a constrained "active" chunk window.
- The forward search was limited by the current samples received (e.g., 200ms look-ahead).
- If a peak's "true" valley occurred beyond this window, the prominence was truncated, leading to lower reported values.

### 2. Envelope Prominence (The Red Lines) (Historical)
The prominence envelopes were calculated by searching across the entire 15.2-second circular buffer.
- **Bug Identified:** The search logic was incorrectly wrap-around searching up to 15 seconds into the past. If a distant historical valley was lower than all recent valleys, it would inflate the envelope height.

## Resolution: Unified 15.2s Global Context
The prominence calculation has been unified and optimized to ensure consistency between detection and visualization by utilizing a shared global context.

### 1. Unified Global Algorithm
A centralized helper function, `calculate_prominence_global`, now implements the "lowest valley until higher ground" logic. This function operates directly on the 15.2-second circular `dynamic_smoothings` buffer, ensuring that:
- The search stops only when a value higher than the target index is reached or the 15.2s context boundary is hit.
- The absolute minimum (lowest valley) within those bounds is used as the prominence base.

### 2. Synchronization
Both peak detection and envelope generation now call `calculate_prominence_global`.
- **Peak Detection:** Now utilizes the full 15.2s historical and look-ahead context available in the circular buffer, matching the "perfect" analysis used for visualization.
- **Visualization:** The circular buffer search bug has been replaced with the same unified global logic, ensuring the red-shaded envelopes precisely represent the numerical prominence values.

## Conclusion
By unifying the logic and utilizing the full 15.2-second global context, the numerical prominence values in the debug console now precisely match the visual scaling of the prominence envelopes. The decorrelation has been fully resolved by ensuring both subsystems utilize the exact same calculation and context window.

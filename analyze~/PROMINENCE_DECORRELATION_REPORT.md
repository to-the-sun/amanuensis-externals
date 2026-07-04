# Technical Report: Prominence Decorrelation in `analyze~` Visualizer

## Overview
A decorrelation was observed between the numerical prominence values used for peak detection and the heights of the red-shaded prominence envelopes in the `analyze_files.py` visualizer. This report details the implementation of a "first valley" prominence logic that synchronizes both systems while maintaining locality and real-time performance.

## Root Cause: Search Window Disparity
Previously, the visual prominence envelopes were searching across the entire 15-second circular buffer to find the lowest valley. If a very low valley existed 10 seconds ago, it would inflate the prominence height of a current peak, even if there were higher ground or other valleys in between.

## The Solution: First Valley Prominence
Both the peak detection algorithm and the visualizer have been updated to use a consistent "first valley" search strategy.

### Logic Details
When calculating the prominence of a peak (at time *T*):
1. **Backward Search:** The algorithm searches back in time until it either:
   - Encounters a value higher than the peak (higher ground).
   - Encounters a value higher than the previous frame in the search (a valley was found, and the signal is rising again).
2. **Forward Search:** The algorithm searches forward in time until it either:
   - Encounters a value higher than the peak.
   - Encounters a value higher than the previous frame in the search (a valley was found).
3. **Calculation:** The prominence is the peak's height minus the higher of these two "first valleys."

### Why this works:
- **Locality:** By stopping at the first valley, we ensure that the prominence characterizes the *immediate* transient event. A valley from 10 seconds ago cannot "reach" a current peak unless the signal never dropped and then rose again in the intervening time.
- **Consistency:** Since both detection and visualization now use this identical logic, the red envelopes accurately represent the numerical values used to trigger peaks.
- **Latency:** This approach does not require large look-ahead windows. While the search is technically limited by the available data (e.g., 200ms look-ahead), audio transients typically find their "first valley" within a very short duration.

## Conclusion
The prominence calculation is now robustly localized. The "first valley" rule naturally filters out distant historical minimums, ensuring that scaling remains consistent and visuals remain synchronized with the underlying analysis.

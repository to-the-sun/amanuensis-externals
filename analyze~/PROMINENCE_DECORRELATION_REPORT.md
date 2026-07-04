# Technical Report: Prominence Decorrelation in `analyze~` Visualizer

## Overview
A decorrelation was observed between the numerical prominence values displayed in the debug console of `analyze_files.py` and the heights of the corresponding red-shaded prominence envelopes on the graph. This report details the investigation into this discrepancy.

## Core Discrepancy: Windowing and Latency
The primary cause of the decorrelation is a disparity in the temporal window used for prominence calculation during peak detection versus the envelope generation used for visualization.

### 1. Peak Detection Prominence
During real-time analysis (and simulated batch analysis), prominence for peak detection is calculated within the context of the current "active" chunk. Specifically, in `cumulative_transience.c`:
- The analyzer searches backwards and forwards from a candidate peak.
- However, for real-time processing, the "forward" search is constrained by the current samples received.
- In the `analyzer_batch_analyze` loop, the look-ahead was set to 200ms (`sr * 0.2`). If a peak occurs near the end of this window, and its "true" valley (the minimum value before it rises again) occurs 300ms later, the prominence calculation will be truncated. This results in a **lower prominence value** being reported to the console.

### 2. Envelope Prominence (The Red Lines)
The prominence envelopes (the continuous red lines on the graph) are calculated using a different mechanism:
- In `analyzer_analyze_chunk`, the prominence envelope for the entire 15-second circular buffer was being recalculated.
- Because this search utilizes the circular buffer (`dynamic_smoothings`), it has access to both the distant past and (during batch processing) the "future" data relative to many frames.
- **Bug Identified:** The search logic in `analyzer_analyze_chunk` was searching up to 15 seconds into the past across the circular buffer boundaries. If a valley from 10 seconds ago was lower than any recent valley, it would be used as the base for the current frame's prominence, leading to an **inflated envelope height** on the graph.

## Why Increasing Look-ahead Helps (and Its Cost)
Increasing the look-ahead (e.g., to 500ms) ensures that the peak detection logic has a larger "future" window to find the true right-side minimum of a transient.
- **Benefit:** More accurate prominence values that better match the visual transients.
- **Cost:** Higher latency in the Max object. For real-time performance, 200ms is a balance between accuracy and responsiveness.

## Conclusion
The numerical "discontinuity" is primarily a visualization artifact where the red lines were over-estimating prominence due to a circular buffer wrap-around bug, while the peak detection was under-estimating prominence due to the short look-ahead window.

The cosmetic bug in the red lines (the circular buffer search) has been identified, but per user request, only visual styling was implemented in this pass. The numerical discrepancy remains an inherent property of the trade-off between real-time latency and "perfect" look-ahead analysis.

# Reconciliation Report: Cumulative Transience Pipeline

This report details the reconciliation of discrepancies between the Max External (`analyze~.c`) and the Python Visualizer (`analyze_files.py`). All algorithmic logic is unified into the C core (`cumulative_transience.c`).

## 1. Flux Scaling and Normalization
- **Resolved**: Previously, Python reported flux values 10x-100x higher than Max due to memory alignment issues and un-normalized sums.
- **Unified Version**: **Normalized Mel-Sum**. Flux is calculated as the sum of positive changes across 32 Mel bands, divided by 32, keeping values in a stable range (0-20).

## 2. FFT Normalization and Power Scaling
- **Resolved**: Varied gain factors ($1$, $1/N$, or $1/\sqrt{N}$) previously led to magnitude variances.
- **Unified Version**: **$1/N^2$ Power Normalization**. The C core uses a standard Radix-2 FFT where power is normalized by $N^2$, ensuring consistency across all analysis calls.

## 3. Spectral Breathing and Noise Floor
- **Resolved**: "Spectral Breathing" was previously more pronounced in real-time due to local-only normalization.
- **Unified Version**: **Stateful Rolling Max energy with 80dB Clamping**. Every frame is clamped to a floor of `max_mel_db - 80.0`, providing a stable noise floor.

## 4. Thresholding and Peak Detection
- **Resolved**: Max and Python previously used different context window sizes for thresholding.
- **Unified Version**: **999-Millisecond Sub-Window Threshold**. Both environments use a 999ms sub-window (within a 15s cache) to calculate the rolling midpoint of flux.

## 5. Resonance Analysis and Inter-band Synchronization
- **Resolved**: Peaks were previously processed in different orders (sequential vs. chunked), leading to divergent resonance qualifiers.
- **Unified Version**: **Chronological Cross-Band Sorting**. The C core gathers candidates across all bands, sorts them globally, and feeds them into the stateful resonance engine.

## 6. STFT Padding and Seam Artifacts
- **Resolved**: Chunked analysis in Max previously created "dips" at edges due to lack of stateful overlap.
- **Unified Version**: **Internal Overlap Buffer**. The `TransientAnalyzer` combines new audio with the previous block's tail for a seamless STFT.

## 7. Scaling of Y-axis (Visual vs. Console)
- **Resolved**: Console numbers previously mismatched visual spikes because Python was recalculating metrics.
- **Unified Version**: **Synchronized Data Flow**. The Python visualizer receives the *exact* flux envelopes and `PeakResult` objects calculated by the C core.

## 8. Summary of Parameters
| Parameter | Unified Value |
| :--- | :--- |
| **FFT Size** | 2048 |
| **Hop Size** | ~1ms (calculated per SR) |
| **Mel Bands** | 128 (4 analysis bands of 32 each) |
| **Noise Floor** | 80dB below rolling max energy |
| **Peak Threshold** | 999ms rolling midpoint (0.0dB Absolute Floor) |
| **Peak Prominence** | > 999ms Rolling Midpoint |
| **Resonance Lookback** | 5.0 seconds |
| **Context Window** | 15.2 seconds |

By implementing these standards, the system ensures that the visualization is an exact representation of the Max object's real-time analysis.

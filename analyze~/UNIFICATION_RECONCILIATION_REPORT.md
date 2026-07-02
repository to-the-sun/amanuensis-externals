# Unification Reconciliation Report: Cumulative Transience Pipeline

This report details the final reconciliation of discrepancies between the Max External (`analyze~.c`) and the Python Visualizer (`analyze_files.py`). All algorithmic logic has been unified into the C core (`cumulative_transience.c`), and host-specific implementations have been discarded.

## 1. Flux Scaling and Normalization
- **Previous Discrepancy**: Python reported flux values 10x-100x higher than Max.
- **Root Cause**: Memory alignment mismatch in the Cython extension caused Python to read corrupted memory. Additionally, Python was using un-normalized mel-band sums.
- **Unified Version**: **Normalized Mel-Sum**.
- **Details**: Flux is calculated as the sum of positive changes across 32 Mel bands, divided by 32. This keeps Flux values in a stable, human-readable range (typically 0-20). The memory alignment is now fixed, ensuring both systems see identical values.
- **Discarded**: The un-normalized sum and the corrupted Python memory reads.

## 2. FFT Normalization and Power Scaling
- **Previous Discrepancy**: Differences in STFT implementation led to magnitude variances.
- **Root Cause**: Varied gain factors in FFT implementations ($1$, $1/N$, or $1/\sqrt{N}$).
- **Unified Version**: **$1/N^2$ Power Normalization**.
- **Details**: The C core now uses a standard Radix-2 FFT where the power of each bin is normalized by $N^2$ (since the forward FFT has a gain of $N$). This ensures that energy levels are independent of the FFT size and consistent across all analysis calls.
- **Discarded**: Arbitrary scaling factors previously used in the Python/Max divergence.

## 3. Spectral Breathing and Noise Floor
- **Previous Discrepancy**: "Spectral Breathing" (flux values dipping when a loud sound occurs elsewhere) was more pronounced in the real-time stream.
- **Root Cause**: Normalizing the spectrogram relative to the *local* window's peak.
- **Unified Version**: **Stateful Rolling Max energy with 80dB Clamping**.
- **Details**: The C core tracks the `max_mel_db` across the entire session. Every frame is clamped to a floor of `max_mel_db - 80.0`. This provides a stable noise floor that mimics global normalization while remaining compatible with real-time streams.
- **Discarded**: Local-only normalization and the un-clamped noise floor.

## 4. Thresholding and Peak Detection
- **Previous Discrepancy**: Max used an adaptive rolling threshold; Python used a batch pass.
- **Root Cause**: Difference in context window sizes.
- **Unified Version**: **999-Millisecond Sub-Window Threshold**.
- **Details**: Both systems now use a 999-millisecond sub-window (within the 15s cache) to calculate the rolling midpoint of flux. This ensures that the detection threshold is highly localized and identical in both environments.
- **Discarded**: The "Batch" thresholding logic (which had access to future data) and short (2s/6s) real-time windows.

## 5. Resonance Analysis and Inter-band Synchronization
- **Previous Discrepancy**: Peaks were processed in different orders, leading to divergent resonance qualifiers.
- **Root Cause**: Python processed bands sequentially; Max processed them in chunks but without cross-band sorting.
- **Unified Version**: **Chronological Cross-Band Sorting**.
- **Details**: The C core now gathers all peak candidates across all 4 bands within the current 100ms cycle, sorts them into a single global chronological stream, and then feeds them into the stateful resonance engine.
- **Discarded**: Sequential band processing and unsorted chunk processing.

## 6. STFT Padding and Seam Artifacts
- **Previous Discrepancy**: Analysis of 100ms chunks in Max created "dips" at the edges due to STFT windowing (Hanning taper).
- **Root Cause**: Lack of stateful overlap between consecutive audio blocks.
- **Unified Version**: **Internal Overlap Buffer**.
- **Details**: The `TransientAnalyzer` now maintains an internal `overlap_buffer`. When new audio is pushed, it is combined with the tail of the previous block to perform a seamless, continuous STFT.
- **Discarded**: The "Zero-padding" at chunk edges.

## 7. Scaling of Y-axis (Visual vs. Console)
- **Previous Discrepancy**: The console reported `flux` numbers that didn't match the height of spikes on the graph.
- **Root Cause**: Disconnected data paths (Max reported C metrics, Python recalculated them).
- **Unified Version**: **Synchronized Data Flow**.
- **Details**: The Python visualizer now receives the *exact* flux envelopes and `PeakResult` objects calculated by the C core. The Y-axis is dynamically scaled to the `max_peak_value` seen by the C core during the session.
- **Discarded**: Redundant Python onset calculations.

## 8. Summary of Parameters
| Parameter | Unified Value |
| :--- | :--- |
| **FFT Size** | 2048 |
| **Hop Size** | 1ms (precisely calculated per SR) |
| **Mel Bands** | 128 (4 analysis bands of 32 each) |
| **Noise Floor** | 80dB below rolling max energy |
| **Peak Threshold** | 999ms rolling midpoint (0.0dB Absolute Floor) |
| **Peak Prominence** | > 999ms Rolling Midpoint |
| **Resonance Lookback** | 5.0 seconds |

By implementing these unified standards, the distinction between "Incremental" and "Batch" has been moved from the **algorithm** to the **orchestration**, ensuring that what you see on the graph is exactly what the Max object is hearing.

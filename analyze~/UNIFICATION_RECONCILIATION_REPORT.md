# Unification Reconciliation Report: Cumulative Transience Pipeline

This report details the reconciliation of historical discrepancies between the Max External (`analyze~.c`) and the Python Visualizer (`analyze_files.py`). All algorithmic logic has been unified into the C core (`cumulative_transience.c`).

## 1. Flux Scaling and Normalization
- **Historical Discrepancy**: Python previously reported flux values 10x-100x higher than Max due to memory alignment issues and un-normalized sums.
- **Unified Version**: **Normalized Mel-Sum**.
- **Current State**: Flux is calculated as the sum of positive changes across 32 Mel bands, divided by 32. This keeps values in a stable range (typically 0-20). The memory alignment is fixed in the Cython extension.

## 2. FFT Normalization and Power Scaling
- **Historical Discrepancy**: Differences in gain factors between FFT implementations led to magnitude variances.
- **Unified Version**: **$1/N^2$ Power Normalization**.
- **Current State**: The C core uses a Radix-2 FFT where the power of each bin is normalized by $N^2$. This ensures consistency independent of the FFT size.

## 3. Spectral Breathing and Noise Floor
- **Historical Discrepancy**: "Spectral Breathing" was more pronounced in real-time due to local window peak normalization.
- **Unified Version**: **Stateful Rolling Max energy with 80dB Clamping**.
- **Current State**: The C core tracks `max_mel_db` across the entire session. Every frame is clamped to a floor of `max_mel_db - 80.0`. This provides a stable noise floor that mimics global normalization.

## 4. Thresholding and Peak Detection
- **Historical Discrepancy**: Max used an adaptive rolling threshold; Python used a batch pass with different context windows.
- **Unified Version**: **Dynamic Historical Peak-Based Midpoint**.
- **Current State**: Both systems use a dynamic historical peak-based sub-window within the 15s cache to calculate the rolling midpoint of flux. Detection requires `flux > midpoint` and `prominence > midpoint`.

## 5. Resonance Analysis and Inter-band Synchronization
- **Historical Discrepancy**: Peaks were processed in different orders, leading to divergent resonance qualifiers.
- **Unified Version**: **Chronological Cross-Band Sorting**.
- **Current State**: The C core gathers all peak candidates across all 4 bands within the current 100ms cycle and sorts them into a single global chronological stream before resonance processing.

## 6. STFT Padding and Seam Artifacts
- **Historical Discrepancy**: Analysis of 100ms chunks in Max created "dips" at the edges due to lack of stateful overlap.
- **Unified Version**: **Internal Overlap Buffer**.
- **Current State**: The `TransientAnalyzer` maintains an internal `overlap_buffer`. When new audio is pushed, it is combined with the tail of the previous block for a seamless STFT.

## 7. Scaling of Y-axis (Visual vs. Console)
- **Historical Discrepancy**: Console reported `flux` numbers that didn't match the height of spikes on the graph.
- **Unified Version**: **Synchronized Data Flow**.
- **Current State**: The Python visualizer receives the *exact* flux envelopes and `PeakResult` objects calculated by the C core. The Y-axis is dynamically scaled to the `max_peak_value` tracked by the C core.

## 8. Summary of Parameters
| Parameter | Unified Value |
| :--- | :--- |
| **FFT Size** | 2048 |
| **Hop Size** | 1ms (precisely calculated per SR) |
| **Mel Bands** | 128 (4 analysis bands of 32 each) |
| **Noise Floor** | 80dB below rolling max energy |
| **Peak Threshold** | Dynamic historical peak-based rolling midpoint (0.0dB Absolute Floor) |
| **Peak Prominence** | > Dynamic historical peak-based rolling midpoint |
| **Resonance Lookback** | 5.0 seconds |

By implementing these unified standards, the project has eliminated the algorithmic distinction between "Incremental" and "Batch" modes, ensuring that visualization perfectly matches real-time audio analysis.

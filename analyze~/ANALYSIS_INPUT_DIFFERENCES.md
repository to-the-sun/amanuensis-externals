# Analysis Input Harmonization: Python vs. Max Unified Model

This report documents the resolution of technical discrepancies through the implementation of the **Unified 15.2s Windowing Model**. Both the Python offline visualizer and the Max real-time external utilize the same C-core orchestration logic.

## 1. Harmonized `max_peak` Reference
- **Mechanism**: Both environments initialize the `TransientAnalyzer` which dynamically updates its internal `max_peak` state based on the loudest flux encountered.
- **Status**: **RESOLVED**. Both pipelines converge on the same reference, eliminating normalization discrepancies.

## 2. Harmonized Peak Suppression and Lookahead
- **Mechanism**: The C core manages peak detection and utilizes a 200ms lookahead to ensure deterministic peak suppression.
- **Status**: **RESOLVED**. Both environments see the same 15.2s context before committing a peak, ensuring consistent results.

## 3. Synchronized Inter-band Processing Order
- **Mechanism**: `analyzer_analyze_chunk` performs a **global chronological sort** of all peaks across all 4 bands before resonance processing.
- **Status**: **RESOLVED**. Resonance energy accumulation is now identical in both environments.

## 4. Unified Resonance Context
- **Mechanism**: The 15.2s window ensures that the 5-second resonance lookback always has valid historical data, even at processing boundaries.
- **Status**: **RESOLVED**. Python and Max see the same historical peaks for every resonance calculation.

## 5. Synchronized Spectral Breathing
- **Mechanism**: Both pipelines use the same sliding 15.2s window and 80dB clamping logic for STFT normalization.
- **Status**: **RESOLVED**. Spectral breathing is now harmonized and identical across both host environments.

## 6. Standardized Temporal Resolution
- **Mechanism**: Python resamples audio to 44.1kHz to match the Max environment's standard (unless otherwise specified). The `TransientAnalyzer` calculates precise frame durations based on the sample rate.
- **Status**: **RESOLVED**.

## 7. Consistent Window Edge Handling
- **Mechanism**: The unified 15.2s window + 200ms lookahead protects the "active" analysis zone from STFT tapering artifacts.
- **Status**: **RESOLVED**.

# Analysis Input Harmonization: Python vs. Max Unified Model

This report documents the resolution of technical discrepancies through the implementation of the **Unified 15.2s Windowing Model**. Both the Python offline visualizer and the Max real-time external now utilize the same C-core orchestration logic.

## 1. Harmonized `max_peak` Reference
- **Mechanism**: Both environments initialize with `max_peak = 1.0` and utilize the `analyzer_analyze_chunk` function, which dynamically updates the analyzer's `max_peak` state based on the loudest flux encountered in the current 15.2s window.
- **Status**: **RESOLVED**. Both pipelines converge on the same local reference, eliminating normalization discrepancies.

## 2. Harmonized Peak Suppression and Lookahead
- **Mechanism**: The C core now manages peak detection within the 15.2s window and utilizes a 200ms lookahead to ensure deterministic peak suppression.
- **Status**: **RESOLVED**. "Double-hits" are eliminated as both environments see the same 15.2s context before committing a peak to the resonance engine.

## 3. Synchronized Inter-band Processing Order
- **Mechanism**: `analyzer_analyze_chunk` performs a **global chronological sort** of all peaks across all 4 bands before processing.
- **Status**: **RESOLVED**. Resonance energy accumulation is now identical in both environments, regardless of which band a transient occurs in.

## 4. Unified Resonance Context
- **Mechanism**: The 15.2s window ensures that the full 5-second resonance lookback always has valid historical data, even at window boundaries.
- **Status**: **RESOLVED**. Python and Max now see the exact same historical peaks for every resonance calculation.

## 5. Synchronized Spectral Breathing
- **Mechanism**: Both pipelines now use a sliding 15.2s window for STFT normalization.
- **Status**: **RESOLVED**. While spectral breathing still exists as a property of the sliding window, it is now identical in both Python and Max, ensuring visual and algorithmic parity.

## 6. Standardized Temporal Resolution
- **Mechanism**: Python now resamples all audio to 44.1kHz to match the Max environment's standard. The `TransientAnalyzer` calculates precise `frame_duration_ms` based on the sample rate.
- **Status**: **RESOLVED**. Physical resonance durations are now perfectly synchronized.

## 7. Consistent Window Edge Handling
- **Mechanism**: The unified 15.2s window + 200ms lookahead ensures that the "active" zone being analyzed is always protected from STFT tapering artifacts.
- **Status**: **RESOLVED**. Edge artifacts no longer cause discrepancies in peak detection sensitivity between the two environments.

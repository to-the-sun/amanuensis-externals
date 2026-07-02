# Analysis Input Harmonization: Python vs. Max Unified Model

This report documents the resolution of technical discrepancies through the implementation of the **Unified 15.2s Windowing Model**. Both the Python offline visualizer and the Max real-time external now utilize the same C-core orchestration logic.

## 1. Harmonized `max_peak` Reference
- **Mechanism**: Both environments initialize with `max_peak = 1.0`. The `analyzer_analyze_chunk` function dynamically updates the analyzer's `max_peak` state based on the highest flux encountered in the current session.
- **Status**: **RESOLVED**. Both pipelines converge on the same reference, eliminating normalization discrepancies.

## 2. Harmonized Peak Suppression and Lookahead
- **Mechanism**: The C core manages peak detection within a 15.2s cache and utilizes a 200ms lookahead (by processing chunks that are 200ms behind the latest received audio) to ensure deterministic peak suppression.
- **Status**: **RESOLVED**. "Double-hits" are eliminated as both environments see the same context before committing a peak to the resonance engine.

## 3. Synchronized Inter-band Processing Order
- **Mechanism**: `analyzer_analyze_chunk` performs a **global chronological sort** of all peak candidates across all 4 bands before they are processed by the resonance engine.
- **Status**: **RESOLVED**. Resonance energy accumulation is now identical in both environments.

## 4. Unified Resonance Context
- **Mechanism**: The 15.2s cache ensures that the 5-second resonance lookback always has valid historical data, even at window boundaries.
- **Status**: **RESOLVED**. Python and Max now see the exact same historical peaks for every resonance calculation.

## 5. Synchronized Spectral Breathing
- **Mechanism**: Both pipelines use a sliding 15.2s window for STFT normalization, clamping the noise floor to 80dB below the session's rolling max energy.
- **Status**: **RESOLVED**. Spectral breathing is now identical in both environments.

## 6. Standardized Temporal Resolution
- **Mechanism**: Python resamples all audio to 44.1kHz to match the Max environment. The `TransientAnalyzer` calculates precise `frame_duration_ms` (e.g., 0.9977ms for 44.1kHz) based on the sample rate to prevent drift.
- **Status**: **RESOLVED**.

## 7. Consistent Window Edge Handling
- **Mechanism**: The unified 15.2s cache + 200ms lookahead ensures that the "active" zone being analyzed is always protected from STFT tapering artifacts by utilizing an internal overlap buffer.
- **Status**: **RESOLVED**.

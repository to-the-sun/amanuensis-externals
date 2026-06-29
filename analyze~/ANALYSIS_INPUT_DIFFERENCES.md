# Analysis Input Differences: Python (Offline) vs. Max (Real-time)

This report outlines the technical discrepancies in how data is prepared and "handed off" to the core C transience algorithm (`cumulative_transience.c`) between the offline Python batch processor and the real-time Max external.

## 1. Global vs. Converging `max_peak` Reference

### Python (Offline)
- **Mechanism**: Performs a pre-analysis pass on the entire audio file using `analyzer_analyze_audio`.
- **Hand-off**: It extracts the absolute maximum flux value from the whole file and passes it as `max_peak_value` during `analyzer_create`.
- **Impact**: The C algorithm's internal normalization (`snapshot *= peak_val / max_peak`) is stable and consistent for every peak in the file.

### Max (Real-time)
- **Mechanism**: Initializes the analyzer with a default `max_peak` of `1.0`. It then dynamically updates this value in its background worker whenever a higher flux peak is encountered.
- **Hand-off**: The C algorithm receives a "converging" maximum.
- **Impact**: Early peaks in a stream (before a true global maximum is reached) are normalized against an artificially low reference. This results in snapshots having higher relative energy than they would in a batch analysis, which in turn inflates the resonance scores for those early peaks.

## 2. Peak Suppression and Minimum Distance

### Python (Offline)
- **Mechanism**: `analyzer_analyze_audio` is called once for the entire duration. The 200ms minimum distance logic is applied in a single global pass.
- **Hand-off**: If two potential peaks are 150ms apart, only the larger one is ever "handed off" to the resonance processor.

### Max (Real-time)
- **Mechanism**: Re-runs peak detection every 100ms on a sliding window. The detector only suppresses peaks relative to others *within the same 6s window*.
- **Hand-off**: If a larger peak appears in a *later* window pass that was not present in the pass where a smaller, nearby peak was first detected and processed, both peaks will have been "handed off" to the C core.
- **Impact**: Real-time analysis can suffer from "double hits" where multiple transients are processed within the 200ms exclusion zone because they weren't visible to the detector at the same time.

## 3. Inter-band Processing Order

### Python (Offline)
- **Mechanism**: Processes the entire file frame-by-frame. If multiple bands have peaks at different times, they are handed to the C core in strict chronological order.
- **Hand-off**: A peak at 200ms in Band 0 will always "see" the resonance energy of a peak at 100ms in Band 1.

### Max (Real-time)
- **Mechanism**: Processes the current window band-by-band (`for b < MAX_BANDS`).
- **Hand-off**: All peaks for Band 0 are processed, then all for Band 1, and so on.
- **Impact**: A peak at 100ms in Band 1 will be handed to the C core *after* a peak at 200ms in Band 0. Consequently, the 200ms peak fails to account for the resonance of the 100ms peak because that energy hasn't been added to the buffer yet. This disrupts the chronological accumulation of energy that the resonance algorithm depends on.

## 4. Resonance Context (`all_valid_peak_indices`)

### Python (Offline)
- **Mechanism**: Passes a sorted list of every peak in the entire file to every `analyzer_process_peak` call.
- **Hand-off**: The C core's resonance calculation (`found_peak` loop) checks against every potential transient in the song.

### Max (Real-time)
- **Mechanism**: Only peaks detected within the current 6-second window are passed as valid indices.
- **Hand-off**: The C core only "sees" peaks that exist within the current window.
- **Impact**: Although the C core only looks back 5 seconds, any peak that falls just outside the 6s window but within the 5s lookback (due to windowing or latency) will be missing from the resonance calculation in Max, whereas it would be present in Python.

## 5. Mel Spectrogram Normalization (Spectral Breathing)

### Python (Offline)
- **Mechanism**: Normalizes the Mel spectrogram relative to the loudest frame in the *entire file*.
- **Hand-off**: The flux envelope (`env_ptr`) provided to the peak processor is derived from a globally stable spectral representation.

### Max (Real-time)
- **Mechanism**: Normalizes the Mel spectrogram relative to the loudest frame in the *current 6-second window*.
- **Hand-off**: As the window slides, the loudest frame within that context changes.
- **Impact**: This causes "spectral breathing," where the flux value for the exact same peak changes as it moves through the sliding window. This fluctuation impacts both peak detection sensitivity and the final `peak_val` handed to the resonance algorithm.

## 6. Temporal Resolution and Resonance Duration

### Python (Offline)
- **Mechanism**: Hop length is `(int)(file_sr * 0.001)`. At 44.1kHz, this is 44 samples.
- **Hand-off**: The C core treats each hop as 1ms.
- **Impact**: 5000 frames (the resonance lookback) equals 220,000 samples, which is actually **4.988 seconds** at 44.1kHz.

### Max (Real-time)
- **Mechanism**: Operates at the system sample rate (e.g., 48kHz). Hop length is 48 samples.
- **Hand-off**: 5000 frames equals 240,000 samples, which is exactly **5.000 seconds**.
- **Impact**: The physical duration of the "resonance window" differs between the two environments if the sample rates don't match. A 44.1kHz file analyzed offline will have a slightly "tighter" resonance window than the same file played back in a 48kHz Max environment.

## 7. STFT Padding and Window Edges

### Python (Offline)
- **Mechanism**: STFT padding occurs only at the absolute start and end of the file.
- **Impact**: Flux envelopes are "clean" for 99% of the song.

### Max (Real-time)
- **Mechanism**: Every 100ms, a new 6-second window is analyzed, with STFT padding at *both* ends.
- **Hand-off**: The flux envelope handed to the C core has ~23ms of zero-tapered data at the window boundaries.
- **Impact**: While the 6s window hides these artifacts from the 5s resonance lookback, they still affect the peak detector's ability to identify transients at the very edges of the sliding window, potentially delaying the first detection of a new peak.

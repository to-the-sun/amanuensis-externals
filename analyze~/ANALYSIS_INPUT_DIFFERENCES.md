# Analysis Input Differences: Python (Offline) vs. Max (Real-time)

This report outlines the technical discrepancies in how data is prepared and "handed off" to the core C transience algorithm (`cumulative_transience.c`) between the offline Python batch processor and the real-time Max external.

## 1. Global vs. Converging `max_peak` Reference (Detail)

The `max_peak` value is a critical scaling factor used by the C core to normalize the resonance snapshots (`snapshot *= peak_val / max_peak`).

### Python (Offline)
- **Mechanism**: Performs a pre-analysis pass on the entire audio file. It determines the true global maximum flux value of any peak found in the song.
- **Hand-off**: This global constant is passed to `analyzer_create`.
- **Impact**: Every peak in the file, from the first second to the last, is normalized against the same stable reference. This ensures that a transient at the beginning of a song has the same relative impact on the resonance buffer as an identical transient at the end.

### Max (Real-time)
- **Mechanism**: The analyzer begins with a default `max_peak` of `1.0`. As it processes the audio stream, it tracks the highest flux value it has seen *so far*.
- **Hand-off**: The C core receives this "high-water mark."
- **Impact**: If a song starts quietly and has a massive peak at the 2-minute mark, all transients in the first two minutes will be normalized against an artificially low `max_peak`. This results in the C core seeing much "hotter" snapshots than it would in a batch analysis, leading to inflated resonance scores and a more volatile `accumulated_buffer` until the true global peak is encountered.

## 2. Minimum Peak Distance and Boundary Suppression (Detail)

The peak detection logic implements a 200ms "exclusion zone" where a smaller peak cannot occur immediately after a larger one.

### Python (Offline)
- **Mechanism**: The 200ms rule is applied once across the entire continuous flux envelope.
- **Impact**: If a peak occurs at `t=1000ms`, the detector is guaranteed to suppress any smaller peaks until `t=1200ms`.

### Max (Real-time)
- **Mechanism**: Peak detection is re-run every 100ms on a sliding window. The detector has no memory of peaks found in previous window passes.
- **Hand-off**: Max uses a `last_peak_frame` check to prevent processing the *same* physical peak twice, but it does not track the *influence* of that peak on its neighbors across window boundaries.
- **Impact**: If a large peak is detected at the very end of Window A, and a smaller peak appears at the start of Window B (separated by only 50ms), the detector in Window B will not "see" the peak from Window A and will therefore fail to suppress the smaller one. This leads to "double-triggering" in real-time that is physically impossible in the batch implementation.

## 3. Inter-band Processing Order

The `cumulative_transience` algorithm is stateful; the resonance score of a peak depends on the energy already present in the `accumulated_buffer`.

### Python (Offline)
- **Mechanism**: Sorts every peak from all four frequency bands into a single chronological list.
- **Hand-off**: Peaks are handed to the C core one-by-one in the exact order they occur in time.
- **Impact**: A peak at 500ms in the Treble band will correctly "see" the resonance contribution of a peak at 450ms in the Bass band.

### Max (Real-time)
- **Mechanism**: The `analyze_worker_task` iterates through the analysis results band-by-band (`for b < MAX_BANDS`).
- **Hand-off**: It processes all peaks for the Sub-Bass band, then all for Bass, then Mid, then Hi.
- **Impact**: If a Bass peak occurs at 100ms and a Treble peak occurs at 50ms, the Max external will hand the 100ms peak to the C core *first* because it belongs to a lower band index. The Treble peak at 50ms will then be processed "out of time," failing to contribute its energy to the Bass peak's resonance score and potentially receiving an incorrect score itself.

## 4. Rolling Threshold Context Window

The peak detection algorithm utilizes a **15-second rolling threshold** to determine if a flux spike is significant relative to the recent local average.

### Python (Offline)
- **Hand-off**: The C core is provided with the full duration of the audio file, allowing the 15-second threshold to "warm up" and maintain a full history.

### Max (Real-time)
- **Hand-off**: The `analyze~` object currently extracts only a **6-second** context window from its circular buffer.
- **Impact**: The peak detector in the C core is effectively "blind" to any audio older than 6 seconds. This forces the 15-second rolling threshold to operate on a truncated history, leading to different peak detection results (and thus different inputs to the resonance algorithm) compared to the 15-second history available in the Python implementation.

## 5. Inter-song Buffer Persistence

### Python (Offline)
- **Mechanism**: A fresh `TransientAnalyzer` is created and destroyed for every individual audio file.
- **Impact**: The resonance buffer always starts at zero for every song.

### Max (Real-time)
- **Mechanism**: The `analyze~` object maintains a single persistent `TransientAnalyzer` state as long as the object exists in the Max patch.
- **Impact**: If a user plays "Song A" and then immediately plays "Song B," the resonance energy and `max_peak` from Song A are still present in the analyzer's memory. Song B will be analyzed using the historical context of Song A, leading to results that are entirely different from a standalone analysis of Song B.

## 6. Temporal Resolution and Frame Drift

### Python (Offline)
- **Mechanism**: Hop length is calculated as `(int)(file_sr * 0.001)`. At 44.1kHz, this is 44 samples (~0.9977ms).
- **Hand-off**: The C core treats each hop as exactly 1.0ms. After 5000 frames (the resonance lookback), the audio has progressed by **4988ms**.

### Max (Real-time)
- **Mechanism**: Operates at the system sample rate (e.g., 48kHz). Hop length is exactly 48 samples (1.0ms).
- **Hand-off**: 5000 frames equals exactly **5000ms** of audio.
- **Impact**: The physical duration of the "resonance window" is slightly different. A 44.1kHz file analyzed offline has a "tighter" resonance window than the same file analyzed in a 48kHz real-time environment, causing the snapshots to be sampled at slightly different temporal positions.

## 7. STFT Padding and Window Edges

### Python (Offline)
- **Mechanism**: STFT padding occurs only at the absolute start and end of the file.

### Max (Real-time)
- **Mechanism**: Every 100ms, a new 6-second window is analyzed, with STFT zero-padding applied at *both* ends of the window.
- **Impact**: The flux envelope handed to the C core has ~23ms of zero-tapered data at the window boundaries. While the 6s window is large enough to "hide" these artifacts from the 5s resonance lookback, they still affect the peak detector's ability to identify transients at the very edges of the sliding window, potentially missing peaks that would be found in the batch version.

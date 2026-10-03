# Detailed Technical Report: Downsampled `cum_history` Envelope Slices

## Executive Overview

In the `analyze~/python/offline` subsystem, `offline_transience.py` produces a field named `cum_history` for every segment in `segments_data`. `cum_history` represents a normalized 300-point 1D float array containing the sliced cumulative spectral transience / onset envelope curve for that specific segment.

This report explains in detail:
1. What `cum_history` represents mathematically and computationally.
2. How `cum_history` is calculated from multi-band transience onset envelopes.
3. Why it is downsampled to exactly 300 points.
4. Its intended visual purpose and current status in the HTML report pipeline.

---

## 1. What `cum_history` Represents

When audio is processed through the cumulative transience core, the C audio analyzer splits the signal into multiple frequency spectral bands (typically 4 sub-bands: low, low-mid, high-mid, high) and computes a frame-by-frame spectral flux / onset envelope (`onset_envs`) for each frequency band.

`cum_history` (short for Cumulative History Envelope Slice) captures the aggregate sum of these spectral transience envelopes over the exact millisecond time span of a single segment (from `start_ms` to `end_ms`).

Mathematically:
- For every frame `f` in the segment duration `[start_frame, end_frame]`, the multi-band transience flux values are summed across all sub-bands:
  `seg_slice[f] = sum(band_envelope[f] for band_envelope in onset_envs)`

---

## 2. Calculation and Resampling Method

In `offline_transience.py`, the calculation steps are:

1. **Frame Conversion**: Convert millisecond boundaries (`start_ms` and `end_ms`) to audio frame indices (`start_frame` and `end_frame`).
2. **Sub-Band Summation**: Sum the envelope arrays across all active frequency bands over the frame range `[start_frame, end_frame]` to create `seg_slice`.
3. **Linear Interpolation / Downsampling**: Resample `seg_slice` to a uniform array of 300 points using 1D linear interpolation (`np.interp`):
   ```python
   cum_hist_pts = np.interp(
       np.linspace(0, 1, 300),
       np.linspace(0, 1, len(seg_slice)),
       seg_slice
   ).tolist()
   ```

---

## 3. Why `cum_history` Is Downsampled to 300 Points

### A. Segment Duration Normalization
Different segments across an audio file or across different songs have varying absolute frame counts depending on bar length and sample rate:
- A 200 ms segment at 44.1 kHz (1 ms hop) contains ~200 frame points.
- A 2500 ms segment contains ~2,500 frame points.

Downsampling/resampling every segment envelope to a fixed length of 300 points normalizes the spatial resolution of the dataset across segments of any length.

### B. Fixed Canvas Render Grid & Performance Optimization
In browser-based GUI inspectors (such as HTML5 Canvas or Raylib views), zoomed-in segment inspector panels render on fixed-width canvas areas (typically 300 to 1,150 pixels wide).
- 300 points provides 1 sample point per 1-4 horizontal pixels on standard displays.
- Keeping `cum_history` at 300 floating-point numbers per segment prevents DOM/JSON memory bloat while preserving smooth curve contours for visual rendering.

---

## 4. Intended Visual Purpose and Current Status

### A. Intended Visual Purpose
`cum_history` was designed to allow zoomed-in segment inspector panels (such as Section 2 in the interactive report) to render the underlying multi-band transience energy curve behind or beneath the audio waveform, giving visual context on where transience bursts occur within each segment.

### B. Current Status in `pattern_finder.py`
In the current implementation of `pattern_finder.py`:
- `pattern_finder.py` extracts raw PCM audio slices for each segment and computes min/max audio waveform envelopes (`waveform_min` and `waveform_max`) for 2D audio rendering inside `drawSegmentBox()`.
- `cum_history` is computed in `offline_transience.py` and included in `segments_data`, but `drawSegmentBox()` in `pattern_finder.py` currently relies on `waveform_min`/`waveform_max` for the main segment display and real-time buffer streaming (`snapshots.bin`) for Section 3 (Accumulated History Buffer).

`cum_history` remains available in `segments_data` as a lightweight 300-point spectral transience envelope profile for offline analysis tools and visualizers.

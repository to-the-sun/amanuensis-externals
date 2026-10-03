# HTML Inspector Data Loading and Size Analysis Report

## Executive Summary
Analysis of `04 transient treble [2026-06-24 181817].wav` revealed why the generated HTML report file reached **139.3 MB** and took several seconds to load:

The root cause was **dual redundant JSON serialization of 15,001-element float arrays** inside the HTML `<script>` tags.

While audio is correctly referenced via relative path on disk (`.wav`) and binary snapshots are saved to `snapshots.bin`, Python's `ct.analyze_audio()` attaches a 15,001-element `snapshot` list of Python floats to every peak in `pass2_res['peaks']`. These full 15,001-element snapshot lists were being serialized into JSON text in **two separate places**:
1. Inside the global `pass2_peaks_js` array.
2. Inside `segments_js` (where every segment dictionary contains a `peaks` list with full snapshot arrays).

For a 60-second audio track with 253 detected peaks:
* `253 peaks * 15,001 floats * 2 serializations` = **7,590,506 floating-point numbers** formatted as ASCII text strings inside the HTML file.
* Converting 7.5 million floats to JSON string text produces **~139.3 MB of text**.

---

## Breakdown of Files and Data Loading Behavior

### 1. Data Embedded Directly in the HTML File (Loaded Immediately upon opening)

When opening `<audio_stem>_pattern_analysis.html`, the browser parses all text between `<script>` tags at load time:

| Data Payload Component | Immediate Size Impact | Purpose |
| :--- | :--- | :--- |
| **HTML DOM & CSS Styling** | ~15 KB | Layout containers, metrics cards, segment inspector boxes |
| **JavaScript Graphics Engine** | ~25 KB | Canvas 2D renderers for waveforms, playhead tracking, cursor badges |
| **Waveform Map Envelope** | ~20 KB | 1,500-point min/max envelope array for the top overview canvas |
| **Peak Metadata (`pass2Peaks`)** | **~69.6 MB** *(before fix)* | JSON array of 253 peaks (timestamps, scores, qualifiers, **plus 15,001-element snapshot lists**) |
| **Segment Data (`segmentsData`)** | **~69.6 MB** *(before fix)* | JSON array of segment ratings and constituent peaks (**with duplicate snapshot lists**) |
| **Total HTML File Size** | **~139.3 MB** | **Browser must parse 139 MB of text before rendering!** |

---

## 2. Files Loaded On-Demand or Asynchronously

| Asset File | Size on Disk | When and How It Is Loaded |
| :--- | :--- | :--- |
| **Audio File (`.wav`)** | ~4.6 MB | **On Demand**: HTML5 `<audio src="04 transient treble.wav">` streams audio chunks from disk as playback progresses or when seeking. It is **never** loaded entirely into HTML memory. |
| **Binary Snapshots (`snapshots.bin`)** | ~15.1 MB | **Asynchronous Background Fetch**: Loaded after page renders via `fetch('snapshots.bin')` into `window.snapshotsArrayBuffer`. |
| **Binary Manifest (`manifest.json`)** | ~2.6 MB | Exported on disk for headless C/Python tools. |
| **Binary Stream (`.ctbin`)** | ~32.9 MB | Pure C binary stream for low-level headless processing. |

---

## Why the HTML File Bloated to 139 MB and How to Fix It

### **Root Cause**:
In `pattern_finder.py`, `export_ctbin_assets` successfully exported all full-resolution snapshots into `snapshots.bin` (15.1 MB) and created byte-offset references (`snap_offset`, `snap_len`). However, when building `pass2_peaks_js` and `segments_js` for the HTML template, the 15,001-element `snapshot` list in Python was **not stripped** from `p_copy` or from `segmentsData['peaks']`.

### **Exact Solution**:
Stripping `snapshot` from peak dictionaries prior to `json.dumps()` in both `pass2_peaks_flat` and `segments_transience_data`:
* **`pass2_peaks_js` size**: Drops from 69.6 MB down to **~80 KB** (metadata only: time, score, qualifiers, `snap_offset`, `snap_len`).
* **`segments_js` size**: Drops from 69.6 MB down to **~30 KB** (segment indices, ratings, peak metadata without snapshots).
* **Final HTML File Size**: Drops from **139.3 MB down to ~150 KB** (a 1,000x reduction).

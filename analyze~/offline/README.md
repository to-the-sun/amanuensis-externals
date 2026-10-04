# Standalone Offline Cumulative Transience Analyzer & Exporter

This directory contains the pure C implementation of the offline transience analyzer and HTML report exporter (`ct_analyze_headless`).

## Architecture

The C pipeline operates in two passes without Python dependencies:

1. **Pass 1 (15,000ms Rolling Window)**:
   - Performs transience analysis across 4 frequency bands using a 15,000ms window (`pass1_window_ms = 15000`).
   - Identifies high point change events and determines the longest stable high point (`best_bar_length_ms`).
   - Tracks the longest contiguous run/duration of `best_bar_length_ms`.
   - Captures the exact 15,000ms cumulative history buffer state and demarcation line at the midpoint of this longest stable high point duration.
   - Exports `midpoint_snapshot` payload (`midpoint_time_s`, `longest_hp_val_ms`, `demarcation_line`, `buffer`) in `report_data.json` and `report_data.js`.

2. **Pass 2 (Adaptive Window = `min(15000, best_bar_length * 2)`)**:
   - Re-analyzes the audio with the optimized window size for peak detection, pattern identification, and segment scoring.
   - Copies the target audio file into the destination folder so the interactive HTML report plays the local copied instance.
   - Scans for and copies any matching text files whose names match the audio file except starting with "passes" instead of "palette" into the destination folder.
   - Exports binary assets (`.ctbin`, `snapshots.bin`), Base64 snapshot fallbacks (`snapshots.js`), structured JSON data (`manifest.json`, `report_data.json`, `report_data.js`), pattern WAV slices, and an interactive HTML report (`<stem>_pattern_analysis.html`).

## Generated HTML Report Layout

The interactive report (`<audio_stem>_pattern_analysis.html`) features:

1. **Top Section: Longest Stable High Point Midpoint History Buffer**:
   - Canvas graph (`#midpointBufferCanvas`) displaying the 15,000ms cumulative history buffer at the midpoint of the longest stable high point duration.
   - Interactive drag-to-zoom and right-click-reset capabilities.

2. **Section 1: Interactive Waveform & Pattern Map**:
   - Overall audio waveform, color-coded pattern spans, high point change markers, and audio seek interactions.

3. **Section 2: Zoomed-In Segment Inspector**:
   - Vertically stacked segment panels (`boxPrev`, `boxCurr`, `boxNext`).
   - Dynamic pattern color-coding: segment borders dynamically highlight using the color of the pattern they belong to, and cleanly revert back to neutral borders when a pattern ends.

4. **Section 3: Real-Time Accumulated History Buffer**:
   - Real-time history buffer graph updating during audio playback with peak markers, qualification lines, tolerance bands, and drag-to-zoom controls.

## Compilation

To compile the standalone analyzer executables:

- **Linux**: `make` (produces `ct_analyze_headless`)
- **Windows (64-bit Cross-Compilation)**: `x86_64-w64-mingw32-gcc -O3 -I. -Wall -Wextra ct_analyze_headless.c ct_exporter.c cumulative_transience.c -o ct_analyze_headless.exe -lm`

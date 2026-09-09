# Weaver Offset Bug and Resolution Technical Report

## Overview
During multi-pass performances where audio spans are continually generated across repeated song loops, `weaver~` was accurately identifying bars to play in its verbose logging messages but intermittently failing to render audio into stem buffers. This resulted in silent bars appearing unpredictably on successive loop passes.

## Root Cause Analysis
The issue stemmed from how `buildspans` recorded the global `offset` property in `transcript.json` relative to `loop_start`:

1. **Loop Start and Most Negative Bar in Note Absolutes:**
   `buildspans` bakes both `loop_start` and `most_negative_bar` directly into incoming raw note timestamps (`looped_absolute = raw_absolute + loop_start + most_negative_bar`) so that notes occurring across performance iterations are placed in correct chronological sequence and quantized properly during rebar calculations (`val = abs_val - offset`).

2. **Unadjusted Offset Storage:**
   Previously, when a new global offset was set on inlet 2 of `buildspans`, the raw offset value was stored directly into the bar's dictionary entry without subtracting the performance loop offset.

3. **Out-of-Bounds Palette Read in Weaver:**
   When `weaver~` renders audio for a bar, it calculates the palette source sample timestamp using the formula:
   `src_ms = offset + current_scan`

   Because `current_scan` reflects absolute song playback time (which includes `loop_start`, e.g., 500,000 ms), an unadjusted offset meant `src_ms` grew linearly with each loop pass. Once `src_ms` exceeded the total duration of the palette WAV buffer (for example, 30,000 ms), `weaver~` evaluated `in_bounds` as 0 and output silence, even though the bar trigger itself was valid.

4. **Direct Loop Start Offset Subtraction:**
   `stored_offset` is calculated directly by subtracting `loop_start` from the incoming offset:
   `stored_offset = raw_offset - loop_start`

   Note `absolutes` take both `loop_start` and `most_negative_bar` into account directly when calculating `looped_absolute`.

## Solution Implemented

### 1. Buildspans Inlet 2 Interface Update
Inlet 2 of `buildspans` was updated to accept a three-item list:
`[offset, loop_start, most_negative_bar]`
If items 2 or 3 are omitted, they default to 0.0.

### 2. Direct Loop Start Subtraction and Absolute Timestamp Calculation
Inside `buildspans_do_offset`, `buildspans` computes:
`stored_offset = raw_offset - loop_start`

The `stored_offset` is stored as the `offset` property in the transcript dictionary and used for active span tracking, while `buildspans_calc_looped_absolute` bakes both `loop_start` and `most_negative_bar` directly into note `absolutes`.

### 3. Real-Time Telemetry and Debug Inspector
`weaver~` was updated to output UDP telemetry on TCP/UDP port 8999, reporting source millisecond playback position, frame indices, source buffer length, bounds status (`in_bounds`), and transcript presence. A Raylib-based OpenGL visualizer (`weaver_inspector.py`) was created to render real-time palette buffer waveforms, virtual Max buffer~ boundaries, playhead cursors, and math equation overlays.

## Verification
With `stored_offset` correctly accounting for `loop_start`, `src_ms` stays anchored within the valid time bounds `[0.0, palette_duration]` regardless of how far out in song time playback progresses. Visual verification in `weaver_inspector.py` confirmed that `in_bounds` remains 1 across repeated loop cycles and `weaver~` consistently writes audio into stem buffers.

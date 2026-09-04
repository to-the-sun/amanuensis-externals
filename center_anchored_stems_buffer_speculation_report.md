# Technical Report: Speculating Center-Anchored Stems Buffers and Code Base Simplification

## Executive Summary

As the system evolved to support bidirectional song expansion—allowing bars and audio to grow in the negative time direction as well as the positive time direction—external objects and Max patchers had to contend with mapping negative time coordinates to non-negative array indices in standard Max audio buffers (`buffer~` and `polybuffer~`).

The historical approach relied on a dynamic frame shifting variable, `most_negative_bar`. Whenever new negative bars were prepended to the transcript dictionary or recorded into stems buffers, the entire reference frame of the song shifted. Every time `most_negative_bar` moved further into the negative direction (for instance, moving from -4000 ms to -8000 ms), offset values across transcripts, loop boundaries, read pointers, and sample positions had to be recalculated or adjusted dynamically across all C external objects (`weaver~`, `crucible`, `smartloop~`) and Python visualizers (`visualizer.py`).

This report explores an architectural alternative: **Center-Anchored Stems Buffers**. Since Max audio buffers already reserve large allocations (for example, 10 to 30 minutes of stereo or multi-channel audio memory), the song origin (time = 0.0 ms) can simply be anchored at the exact midpoint of the stems buffer (e.g., sample frame offset corresponding to 15 minutes into a 30-minute buffer). Audio recording and playback can then expand outwards in either direction relative to this fixed anchor point.

---

## 1. Problem Statement: Dynamic Framing Shift Under Negative Growth

In standard audio buffer architectures, sample index 0 corresponds to time 0.0 ms. When a song starts at time 0.0 ms and grows exclusively forward, the relationship between song time $T$ and buffer frame $F$ is linear and static:

`sample_frame = (song_time_ms * sample_rate) / 1000.0`

However, when introducing negative time bars (such as lead-ins, count-ins, or prepended song sections), negative time coordinates cannot directly index into standard Max buffers. Under the dynamic `most_negative_bar` approach, the system maps song time to buffer frame using:

`sample_frame = ((song_time_ms - most_negative_bar) * sample_rate) / 1000.0`

### Pain Points of the Dynamic Frame Shift Approach

1. **Cascading State Re-computation**:
   Whenever a new negative bar is added, `most_negative_bar` changes (e.g., from -4000.0 ms to -8000.0 ms). This change triggers a cascade across the entire system:
   - `crucible` must update the `offset` key of newly synthesized or updated stem bars to `-most_negative_bar`.
   - `weaver~` must update its internal track cache and recalculate vector scan times (`ramp_in + most_negative_bar`) to prevent crossfade glitches.
   - `smartloop~` must detect shifts in `most_negative_bar` to adjust its last recorded sample values and prevent false jump/loop detections.
   - `visualizer.py` must rescale all track pixel coordinates across the screen.

2. **Buffer Audio Shifting or Truncation**:
   If stems buffers are recorded starting at physical frame 0, prepending negative audio requires either physically shifting existing audio buffer memory rightward or maintaining complex relative offset math in recording patchers.

3. **Multi-Thread Race Conditions**:
   When `most_negative_bar` updates on Max's main thread, audio DSP perform routines running on the high-priority audio thread may process a vector using stale `most_negative_bar` values while dictionary metadata has already updated, causing momentary audio pops or misaligned crossfades.

---

## 2. Concept Overview: Center-Anchored Stems Buffers

Under a **Center-Anchored Stems Buffer** model:
- Stems buffers are allocated with a fixed large duration $D_{total}$ (e.g., 30 minutes, or 79,380,000 samples at 44.1 kHz).
- The song zero point (time = 0.0 ms) is permanently anchored at $D_{center} = D_{total} / 2$ (e.g., 15 minutes, or 39,690,000 samples).
- Any timestamp $T_{ms}$ (positive or negative) maps directly to a fixed, non-shifting buffer frame:

`sample_frame = center_frame + (T_ms * sample_rate / 1000.0)`

For example:
- Time 0.0 ms maps to frame `center_frame`.
- Time -8000.0 ms maps to frame `center_frame - (8000 * SR / 1000)`.
- Time +12000.0 ms maps to frame `center_frame + (12000 * SR / 1000)`.

Because `center_frame` is a constant derived solely from the destination buffer's total frame count, expanding the song in the negative direction never alters the mapping of pre-existing positive or negative timestamps.

---

## 3. Comprehensive Impact and Code Base Simplifications

### 3.1. Impact on `weaver~`

`weaver~` is the primary C external responsible for sample-accurate audio weaving from palette buffers and fallback stem buffers into a destination polybuffer.

#### Current Complexity in `weaver~`
- Maintains `x->most_negative_bar` globally and per-track (`tr->most_negative_bar`).
- Executes `weaver_update_most_negative_bar` whenever transcript dictionary structures change.
- Maps incoming time ramps during perform vectors:
  `vector_time = ramp_in[0] + x->most_negative_bar`
  `current_scan = ramp_in[i] + x->most_negative_bar`
- Calculates stem fallback sample read offsets relative to `x->most_negative_bar`:
  `fallback_offset = -x->most_negative_bar`
  `song_ms_offset = hit.value - x->most_negative_bar`
- Handles track loop bounds remapping for tracks that do not extend as far in the negative direction as the longest track.

#### Simplifications under Center-Anchored Stems Buffers
1. **Elimination of Dynamic Framing Logic**:
   `weaver~` no longer needs to track or calculate `most_negative_bar`.
2. **Direct Fixed Indexing**:
   When reading fallback audio from `stems.[track_id]`, the read frame is simply:
   `read_frame = (buffer_total_frames / 2) + (song_ms * sample_rate / 1000.0)`
3. **Stateless Time Ramps**:
   The incoming time ramp signal on inlet 1 represents absolute song time in milliseconds directly. `weaver~` does not need to add or subtract variable offsets during DSP vector iterations.
4. **Thread-Safe Crossfading**:
   Because buffer frame mappings remain constant, prepending a negative bar to the transcript cannot desynchronize running DSP vector scans.

---

### 3.2. Impact on `crucible`

`crucible` manages incumbent and challenger track dictionaries, performing competitive evaluation, score calculation, span creation, and reach monitoring.

#### Current Complexity in `crucible`
- When rescoring or adding new bars in `@rescore 1` mode, `crucible` scans all existing bars across all tracks to determine `song_min` (`most_negative_bar`).
- It explicitly populates each stem bar's `offset` key in the dictionary with `-most_negative_bar`.
- When negative reach changes, `crucible` fires `min [song_min]` reach notifications out of its reach outlet, forcing downstream patchers and objects to update their frame references.

#### Simplifications under Center-Anchored Stems Buffers
1. **Fixed Zero-Based Offsets**:
   Stem bars no longer require a dynamic `offset` key equal to `-most_negative_bar`. Stems fallbacks automatically map to `center_frame + timestamp`.
2. **Simplified Rescoring & Span Grouping**:
   Contiguous stem spans no longer need re-evaluation or span splitting caused solely by a shift in `most_negative_bar`. Two stem bars with identical palettes remain part of the same contiguous span regardless of how many negative bars are added elsewhere.
3. **Reduced Dictionary Mutations**:
   `crucible` does not need to re-scan track bounds to overwrite offset properties of existing bars when `song_min` expands.

---

### 3.3. Impact on `smartloop~`

`smartloop~` monitors audio playback ramps and transcript dictionary bounds to detect user jumps, manual loops, and automatic wrap-arounds.

#### Current Complexity in `smartloop~`
- Tracks `x->most_negative_bar` and `x->last_most_negative_bar`.
- In `smartloop_perform64`, checks for dynamic changes in `most_negative_bar` to adjust `x->last_val`:
  `x->last_val += (x->most_negative_bar - x->last_most_negative_bar)`
  This adjustment is strictly necessary to prevent false jump detection when the reference frame shifts under a playing position.
- Maps output start and end timestamps for loop outlets:
  `mapped_start = x->current_start - x->most_negative_bar`
  `mapped_end = x->current_end - x->most_negative_bar`

#### Simplifications under Center-Anchored Stems Buffers
1. **Elimination of False Jump Interlocks**:
   Since `center_frame` is constant, adding negative bars while audio is playing will never alter the incoming time ramp value relative to the buffer. The dynamic jump detection code is simplified to pure delta checks without offset compensation.
2. **Direct Outlet Timestamping**:
   `smartloop~` outputs absolute song millisecond values directly without frame mapping subtractions.

---

### 3.4. Impact on Audio Recording Subsystems (Max Patchers)

#### Current Complexity
Recording negative audio stems into a standard Max buffer starting at index 0 requires either:
- Pre-allocating buffer space and manually calculating write pointers with offset math.
- Moving existing audio data inside the buffer whenever recording extends further into negative time.

#### Simplifications under Center-Anchored Stems Buffers
- The recording write pointer in Max (e.g., via `poke~` or `record~`) is calculated directly from the absolute song time:
  `write_frame = center_frame + (time_ms * sample_rate / 1000.0)`
- Recording at time -5000.0 ms writes directly to `center_frame - 220500` (at 44.1 kHz) without touching or shifting data recorded at 0.0 ms or +10000.0 ms.
- Upstream recording logic becomes completely symmetric for negative and positive song expansion.

---

### 3.5. Impact on `visualizer.py`

`visualizer.py` provides real-time Pygame visualization of tracks, bars, spans, ratings, and active note event hashes.

#### Current Complexity in `visualizer.py`
- Scans track dictionary bars to find `most_negative_bar` and `most_positive_bar_plus_len`.
- Computes grid rendering offsets:
  `start_px = margin_left + ((most_negative_bar - song_start) / bar_length) * cell_w`
  `total_time_span = most_positive_bar_plus_len - most_negative_bar`
  `x_pos = start_px + ((rel_ms - most_negative_bar) / total_time_span) * pixel_width`

#### Simplifications under Center-Anchored Stems Buffers
- Absolute bar timestamps in the dictionary (e.g., -4000.0 ms, 0.0 ms, 8000.0 ms) align directly with grid column indices relative to `song_start`.
- The visualizer grid coordinate mapping simplifies to a fixed linear projection from absolute time $T$ to pixel position $X$, eliminating dynamic origin recalculations during live dictionary repopulation.

---

## 4. Summary of Architectural Advantages and Trade-offs

### Advantages

| Area | Dynamic Framing (`most_negative_bar`) | Center-Anchored Stems Buffers |
| :--- | :--- | :--- |
| **Mathematical Complexity** | High (variable offset subtraction in DSP and dictionary) | Low (fixed constant center offset) |
| **DSP Perform Overhead** | Adds offset addition per sample frame | Single fixed offset calculation |
| **Thread Safety** | Risk of race conditions during frame shifts | Inherently thread-safe |
| **Dictionary Stability** | Requires updating bar offset keys on frame shift | Bar properties remain immutable |
| **Code Maintainability** | High coupling across external C objects and Python | Low coupling; objects handle absolute time |

### Memory Trade-offs
- **Buffer Size Requirement**:
  A center-anchored buffer requires sufficient duration on both sides of the origin. For instance, a 30-minute stereo buffer (15 minutes negative capacity, 15 minutes positive capacity) at 44.1 kHz 32-bit float consumes:
  `30 mins * 60 secs * 44100 samples * 2 channels * 4 bytes = ~635 MB`
- **RAM Overhead vs. Modern Hardware**:
  In modern 64-bit Max environments with multi-gigabyte RAM systems, reserving 600 MB to 1 GB of memory per stem polybuffer is negligible compared to the significant reductions in C code complexity, DSP bugs, and state management overhead.

---

## 5. Implementation Roadmap & Transition Strategy

To transition the codebase to center-anchored stems buffers:

1. **Max Patcher Buffer Allocation**:
   Set default stems buffer size to a fixed duration $D_{total}$ (e.g., 30 minutes) and define `center_frame = total_frames / 2`.

2. **C External Updates**:
   - In `weaver~.c`: Remove `most_negative_bar` tracking routines. Replace stem read frame calculations with `center_frame + (song_ms * SR / 1000.0)`.
   - In `crucible.c`: Remove dynamic `most_negative_bar` rescore calculation loops and static offset overrides.
   - In `smartloop~.c`: Remove `most_negative_bar` change detection logic in perform routines.

3. **Documentation & Reference Updates**:
   Update `weaver~.maxref.xml`, `crucible.maxref.xml`, and object documentation to describe the center-anchored origin model.

---

## 6. Conclusion

Shifting from dynamic `most_negative_bar` frame adjustments to **Center-Anchored Stems Buffers** provides a far cleaner, robust, and elegant architecture for bidirectional song growth. It eliminates complex state synchronization across `weaver~`, `crucible`, `smartloop~`, and `visualizer.py`, reduces DSP lock contention, and simplifies audio recording workflows in Max patchers.

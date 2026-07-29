# Speculative Report: Investigation of `weaver~` Negative Territory Looping and Crossfade Oscillations

This report provides an in-depth technical analysis of why the `weaver~` Max external fails to handle looping/bar transitions in negative territory correctly, leading to the rapid, wild crossfade oscillations and offset resets visible in both `debug_visualizer.py` and the Max audio outputs.

---

## 1. Executive Summary

During forward master transport playback across the zero-point of a song, tracks with negative bar bounds exhibit severe visual and sonic instability. Specifically, in the bar just below zero (between `-bar_length` and `0.0` ms), the green ($f_1$) and red ($f_2$) crossfade gain envelopes oscillate rapidly back and forth. Sonically, this results in the same tiny sliver of audio looping repeatedly as the source playback offset resets on every oscillation cycle.

Our investigation traces this behavior to an architectural feedback loop between:
1. **C Truncated Integer Division** in the continuous bar trigger calculation.
2. **The `tr->busy` State Transition Lifecycle**, which temporarily suppresses new bar triggers during active crossfades but immediately unleashes them upon crossfade completion.

*Note: The high-frequency, rapid oscillations have been successfully resolved by replacing C truncated integer division with mathematical floor division. However, a second, distinct class of slower, single-step oscillations/crossfades remains visible on every single bar boundary below zero. This report has been updated to analyze this second phenomenon.*

---

## 2. Analysis of Visual Evidence (Screenshot Diagnostic)

### 2.1 The High-Frequency Oscillation (Resolved)
In the first screenshot, we observed:
* **Track 1 & Track 4 Oscillations:** In the bar between `-3460` ms and `0` ms (the bar just below zero), the red and green crossfade curves form a dense, criss-crossing mesh. This represents a rapid succession of crossfades.
* **Offset Retriggering:** Under normal operation, a crossfade occurs exactly once at a bar boundary to smoothly transition to a new segment. Once completed, the gain levels out (either green at `1.0` and red at `0.0`, or vice-versa), and the audio offset advances linearly. In the negative bar region, however, the crossfades re-trigger repeatedly as soon as they reach their destination, causing the playback offset to reset to the start of the bar.

### 2.2 The Boundary-Crossing Oscillation (New Observation)
In the second screenshot, after implementing mathematical floor division, we observe:
* **Absence of High-Frequency Mesh:** The high-frequency, rapid "stuttering" oscillations are completely resolved. The gain lines between `-1151` ms and `0` ms are clean and flat.
* **Slower, Boundary-Crossing Crossfades:** Single crossfades (criss-crosses) still occur at every single bar boundary below zero, even for tracks and bars that contain no corresponding active audio or labels (e.g. at `-3453` and `-2302` on Track 1).

---

## 3. Root Cause Analysis of High-Frequency Oscillations

### 3.1 C Integer Division vs. Mathematical Floor Division
In C, the `/` operator between two integers truncates toward zero. Under the standard continuous bar hit detection logic in `weaver~.c`:

```c
long long end = r_scan;
long long latest_j = (end / (long long)bar_len) * (long long)bar_len;
```

When we are in positive territory, say `end = 150` with `bar_len = 100`:
* `latest_j = (150 / 100) * 100 = 100`.
* `start = r_last + 1 = 150`.
* `latest_j >= start` ($100 \geq 150$) is **False**. A trigger only occurs when `end` reaches `200` ($200 \geq 200$, which is **True**). This enforces exactly one trigger per bar boundary.

In negative territory, however, say `end = -150` with `bar_len = 100`:
* `latest_j = (-150 / 100) * 100 = -1 * 100 = -100` (due to truncated division rounding `-1.5` up to `-1`).
* `start = r_last + 1 = -150`.
* `latest_j >= start` ($-100 \geq -150$) is **True**!

Because `-100` is algebraically greater than any negative number between `-150` and `-100`, the condition `latest_j >= start` evaluates to **True on every single sample step/millisecond** in that entire window.

### 3.2 The State Transition & `tr->busy` Feedback Loop
The continuous bar trigger check is guarded by `!tr->busy`:

```c
if ((!tr->busy || main_looped) && !tr->waiting_for_dict && r_scan != r_last && bar_len > 0) { ... }
```

When a bar hit is first triggered:
1. `tr->busy` and `tr->waiting_for_dict` are set to `1`.
2. A metadata lookup is requested via the FIFO.
3. The main thread returns the bar's metadata, clearing `tr->waiting_for_dict` and setting `tr->has_pending_data = 1`.
4. The DSP thread initiates a crossfade and moves `tr->control`.
5. Once the crossfade ramps complete ($f_1$ and $f_2$ reach their targets):
   ```c
   int r1_done = (tr->xf.ramp1.toggle > 0.5) ? (f1 <= 0.0) : (f1 >= 1.0);
   int r2_done = (tr->xf.ramp2.toggle > 0.5) ? (f2 <= 0.0) : (f2 >= 1.0);
   if (r1_done && r2_done && !tr->waiting_for_dict) tr->busy = 0;
   ```
6. The moment `tr->busy` drops to `0`, on the very next DSP vector, the continuous trigger condition is evaluated.
7. Because we are in negative territory and `latest_j >= start` is perpetually **True**, the condition immediately evaluates to **True** again.
8. This starts a brand-new bar hit, sets `tr->busy = 1` again, retrieves the same bar metadata, swaps the crossfade direction, and initiates a reverse crossfade.
9. This cycle repeats endlessly, creating the "wild oscillation" of crossfades and continuous offset resets.

---

## 4. Root Cause Analysis of Slower Boundary-Crossing Oscillations

Even with the high-frequency oscillation resolved, a slower, single-crossfade oscillation occurs at *every* bar boundary below zero. This is caused by a bug in the **Empty/Padded Bar Fallback Logic** in `weaver~.c`:

### 4.1 The Over-Aggressive Stems Fallback Bug
In `weaver_audio_qtask`, when a bar hit event is popped, the external looks up the bar's metadata in the dictionary. If the bar is present but lacks a valid palette (or is a padded/empty bar generated during re-barring), it evaluates `palette_exists`:

```c
int palette_exists = 0;
if (palette != _sym_nothing && palette != _sym_dash) {
    // Check if the buffer is loaded and bound
    ...
    palette_exists = 1;
}

if (!palette_exists) {
    char stems_name[64];
    snprintf(stems_name, 64, "stems.%lld", (long long)target_track);
    t_symbol *s_stems = gensym(stems_name);
    t_buffer_ref *stems_ref = buffer_ref_new((t_object *)x, s_stems);

    if (buffer_ref_getobject(stems_ref)) {
        double fallback_offset = hit.value - x->most_negative_bar;
        palette = s_stems;
        offset = fallback_offset;
    } else {
        palette = _sym_dash;
        offset = 0.0;
    }
}
```

### 4.2 Why This Over-Aggressive Fallback Occurs
* **Explicit Silence (`_sym_dash`):** If a bar is explicitly silent (meaning `palette` is `_sym_dash` or `-`), the condition `palette != _sym_nothing && palette != _sym_dash` is **False**, meaning `palette_exists` is set to `0`.
* **Incorrect Override:** Because `palette_exists` is `0`, the logic assumes the palette "failed to load" and looks for the raw track fallback buffer `stems.x`.
* **Sound Generation in Silent Blocks:** If `stems.x` exists in the Max patcher, `weaver~` overwrites `palette` with `stems.x` and sets `offset = fallback_offset`.
* **The Oscillation Cycle:**
  1. A silent gap is encountered (e.g. at `-3453` on Track 1). Since this bar is completely missing from the dictionary, `found_in_dict` is `False`, so it triggers pure silence (`_sym_dash`). The track crossfades to silence.
  2. The next boundary `-2302` is crossed. If this bar is present in the dictionary (e.g. as an empty/padded bar) but has `palette: "-"`, `weaver~` incorrectly overrides it with `stems.1` and triggers playback.
  3. The track crossfades from silence (`_sym_dash` at `-3453`) to `stems.1` at `-2302`.
  4. At `-1151`, a real bar with `2026-7-29-8-31-55.wav` is triggered, causing another crossfade.
  This creates a continuous cycle of crossfades at every single bar boundary below zero.

---

## 5. Loop Discontinuity (`track_looped` / `main_looped`) Failures Below Zero

An additional problem occurs when a track loops in negative territory:
```c
long long start = (track_looped || main_looped) ? 0 : r_last + 1;
```

When a loop occurs, `start` is hardcoded to `0`.
In negative territory, `latest_j` is negative (e.g., `-3460`).
* **Comparison:** Is `latest_j >= start` ($-3460 \geq 0$)? **False**.
* **Outcome:** The loop boundary trigger is completely skipped, causing the track to fail to load the looped bar's metadata altogether, resulting in silence or the previous segment playing out of bounds.

---

## 6. Proposed Solutions

### 6.1 Solution to High-Frequency Oscillations (Implemented)
1. **Mathematical Floor Division:**
   Replace C integer truncated division with true floor division:
   ```c
   long long latest_j = (long long)floor((double)end / bar_len) * bar_len;
   ```
2. **Dynamic Loop Boundaries:**
   Set `start` to `latest_j` during loop events to ensure immediate trigger satisfaction at negative targets:
   ```c
   long long start = (track_looped || main_looped) ? latest_j : r_last + 1;
   ```

### 6.2 Solution to Boundary-Crossing Fallback Oscillations (Proposed)
To prevent the over-aggressive `stems.x` fallback from overriding intended silence/padded bars:
* **Differentiate Missing Palettes from Explicit Silence:**
  Only trigger the `stems.x` fallback if the palette was a *real* palette filename (not `_sym_dash` and not `_sym_nothing`) that failed to load:
  ```c
  int palette_exists = 0;
  int is_explicit_silence = (palette == _sym_nothing || palette == _sym_dash);

  if (!is_explicit_silence) {
      t_buffer_ref *temp_ref = buffer_ref_new((t_object *)x, palette);
      if (buffer_ref_getobject(temp_ref)) {
          palette_exists = 1;
      }
      object_free(temp_ref);
  }

  if (!palette_exists && !is_explicit_silence) {
      // Fall back to stems.x only if a real palette was missing
      ...
  } else if (is_explicit_silence) {
      // Keep explicit silence as _sym_dash
      palette = _sym_dash;
      offset = 0.0;
  }
  ```
This ensures that `palette: "-"` or padded silent bars are played as absolute silence without triggering a crossfade to `stems.x` and causing boundary oscillations.

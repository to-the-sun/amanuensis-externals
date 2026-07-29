# Speculative Report: Investigation of `weaver~` Negative Territory Looping and Crossfade Oscillations

This report provides an in-depth technical analysis of why the `weaver~` Max external fails to handle looping/bar transitions in negative territory correctly, leading to the rapid, wild crossfade oscillations and offset resets visible in both `debug_visualizer.py` and the Max audio outputs.

---

## 1. Executive Summary

During forward master transport playback across the zero-point of a song, tracks with negative bar bounds exhibit severe visual and sonic instability. Specifically, in the bar just below zero (between `-bar_length` and `0.0` ms), the green ($f_1$) and red ($f_2$) crossfade gain envelopes oscillate rapidly back and forth. Sonically, this results in the same tiny sliver of audio looping repeatedly as the source playback offset resets on every oscillation cycle.

Our investigation traces this behavior to an architectural feedback loop between:
1. **C Truncated Integer Division** in the continuous bar trigger calculation.
2. **The `tr->busy` State Transition Lifecycle**, which temporarily suppresses new bar triggers during active crossfades but immediately unleashes them upon crossfade completion.

---

## 2. Analysis of Visual Evidence (Screenshot Diagnostic)

In the provided screenshot, we observe:
* **The Master Timeline Range:** The viewable span runs from approximately `-10380` ms to `17220` ms.
* **Track 1 & Track 4 Oscillations:** In the bar between `-3460` ms and `0` ms (the bar just below zero), the red and green crossfade curves form a dense, criss-crossing mesh. This represents a rapid succession of crossfades.
* **Offset Retriggering:** Under normal operation, a crossfade occurs exactly once at a bar boundary to smoothly transition to a new segment. Once completed, the gain levels out (either green at `1.0` and red at `0.0`, or vice-versa), and the audio offset advances linearly. In the negative bar region, however, the crossfades re-trigger repeatedly as soon as they reach their destination, causing the playback offset to reset to the start of the bar.

---

## 3. Root Cause Analysis

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

## 4. Impact on Audio and Max Stems Buffers

This rapid re-triggering does not just break the visualizer; it severely degrades the physical audio rendering in Max MSP:
* **Constant Offset Anchoring:** In `weaver~`, the actual playback offset in a source buffer is calculated relative to the trigger time `viz_ms`:
  $$\text{src\_ms} = \text{tr->offset}[j] + \text{v\_at\_f}$$
  Where:
  $$\text{tr->offset}[other] = \text{tr->pending\_offset} - \text{tr->viz\_ms}$$
* **Offset Resets:** Because a new trigger is fired as soon as the crossfade completes, `tr->viz_ms` is constantly overwritten with the current transport time, and the playback position is forced back to the beginning of the bar (`tr->pending_offset`).
* **Stuck Loop Audio:** This causes the external to constantly crossfade back and forth while playing only the first few hundred milliseconds of the bar (the duration of the crossfade ramp), resulting in the "stuck loop" stuttering audio reported by users.

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

To fully resolve the negative territory looping issues in `weaver~`:

1. **Replace C Division with Mathematical Floor Division:**
   Implement true floor division for the boundary calculations so that `latest_j` correctly represents the largest multiple of `bar_len` less than or equal to `end`.
   ```c
   long long latest_j = (long long)floor((double)end / bar_len) * bar_len;
   ```
   This ensures `latest_j` is always less than `start` during steady-state forward play, firing the trigger exactly once when crossing a bar boundary.

2. **Dynamically Align Loop Start Boundaries:**
   Instead of setting `start` to `0` during a loop event, dynamically set it to `latest_j`. This ensures that a loop transition to *any* location (positive or negative) immediately satisfies the trigger check and forces a clean state re-evaluation.
   ```c
   long long start = (track_looped || main_looped) ? latest_j : r_last + 1;
   ```

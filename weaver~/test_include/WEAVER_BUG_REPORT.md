# Speculative Bug Report: Wild Crossfade Oscillations Below Zero

This report documents the detailed investigation, codebase analysis, and speculative root cause for the wild crossfade oscillations observed in the `weaver~` object during playback of bars weaving in negative territory (below `0 ms`).

---

## 1. Observed Phenomenon
- **Oscillations in Negative Territory**: When the master playback timeline (`current_scan`) is in negative territory (e.g., from `x->most_negative_bar` at `-13836 ms` up to `0 ms`), some tracks (such as Track 3 in `Untitled.png` and `weaver_bug_present.png`) experience high-frequency oscillations of their crossfade coefficients (`f1`/`f2`), rather than transitioning smoothly and remaining stable.
- **Stability in Positive Territory**: As soon as the timeline crosses `0 ms`, these fader oscillations immediately stop, and the crossfade envelopes become perfectly stable.
- **Track-Specific Differences**: Track 1, Track 2, and Track 4 do not exhibit the same wild oscillations.

---

## 2. Codebase Investigation & Mathematical Analysis

### A. Master Ramp and Track Coordinate Mapping
Inside `weaver_process_vector`, the current absolute time on the master ramp (`current_scan`) is calculated as:
```c
double current_scan = ramp_in[i] + x->most_negative_bar;
```
For `transcript.json`, `x->most_negative_bar` is `-13836.0 ms`. As `ramp_in` increases from `0` to the song length, `current_scan` goes from `-13836.0 ms` to `13836.0 ms`.

To handle tracks that start later than the global minimum timestamp, `weaver~` applies a wrapping/folding mechanism when `current_scan < tr->most_negative_bar`:
```c
double current_scan_for_track = current_scan;
if (current_scan < tr->most_negative_bar) {
    double T_content_length = tr->highest_bar - tr->most_negative_bar + bar_len;
    if (T_content_length > 0) {
        double diff = tr->most_negative_bar - current_scan;
        double wrapped_diff = fmod(diff, T_content_length);
        if (wrapped_diff < 1e-5) {
            current_scan_for_track = tr->most_negative_bar;
        } else {
            current_scan_for_track = tr->highest_bar - (wrapped_diff - bar_len);
        }
    }
}
```

Then, the within-track scan position (`tr_scan`) is computed using `fmod`:
```c
double tr_scan = fmod(current_scan_for_track, tr->track_length);
```

### B. The Root Cause of Discontinuities and False Loops
1. **The Negative Modulo Issue (`fmod` Behavior)**:
   In C, `fmod(x, y)` preserves the sign of the dividend `x`. When `current_scan_for_track` is negative, `tr_scan` is also negative.
   However, `tr->track_length` represents the span of the track from `x->most_negative_bar` up to the track's highest bar timestamp plus one bar length.
   When wrapping or modulo arithmetic is applied to negative numbers, `fmod(current_scan_for_track, tr->track_length)` does not map the values cleanly to a positive loop interval `[0, tr->track_length)`. Instead, it leaves them negative, meaning `tr_scan` has a massive phase shift relative to positive values.

2. **The Discontinuous Jump and False `track_looped` Triggers**:
   Let's trace `current_scan_for_track` for a track like Track 4 (`tr->most_negative_bar = -3459`):
   - At `current_scan = -13836.0`, it wraps to `current_scan_for_track = 3459.0`.
   - As `current_scan` increases towards `-3459.0`, `current_scan_for_track` increases towards `13835.0`.
   - The moment `current_scan` reaches `-3459.0`, the `current_scan < tr->most_negative_bar` condition becomes **false**.
   - Consequently, `current_scan_for_track` **instantly jumps** from `13835.0` back to `current_scan = -3459.0`.

   This instant backwards chronological jump of **17294 ms** causes:
   - `tr_scan` to drop from `13835.0` to `-3459.0`.
   - `r_scan = floor(tr_scan)` to drop dramatically.
   - `track_looped = (r_scan < r_last)` to evaluate to **true**.

   When `track_looped` is true, the track state triggers loop wrap-around logic, which resets faders, snaps ramp states, and forces re-entry into the initial bar trigger logic. This loop-resetting cycle triggers repeatedly and rapidly while weaving below zero, leading to the **wild, high-frequency oscillations** seen on the crossfades.

---

## 3. Proposed Solution
To eliminate these discontinuities and false loop triggers below zero:
1. **Proper Modulo Wrapping for Negative Timeline Coordinates**:
   Implement a robust coordinate wrapping function for both `current_scan_for_track` and `tr_scan` that maps negative timeline coordinates to their correct, positive equivalent within the range of the track's timeline, rather than leaving them negative or using sign-preserving `fmod` directly on negative dividends.

   A safe way to wrap any coordinate `val` into a track's bounds `[min_val, min_val + length)` is:
   ```c
   double wrapped = fmod(val - min_val, length);
   if (wrapped < 0) wrapped += length;
   wrapped += min_val;
   ```
   This ensures perfect mathematical continuity regardless of whether the timeline is in negative or positive territory.

2. **Smooth Boundary Handover**:
   Ensure that transitioning from pre-start wrapped territory to the track's actual start does not cause a backwards phase jump, but instead aligns seamlessly with the chronological progression of the master playhead.

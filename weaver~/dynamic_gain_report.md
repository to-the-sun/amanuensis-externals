# Deep Dive: The `@dynamic_gain` Attribute in `weaver~`

## Executive Summary

The `weaver~` object is a multichannel audio weaving and crossfading DSP external for Max MSP. It reconstructs dynamic stem tracks from slices of source audio palettes dictated by a structured transcript dictionary.

One of the key features of `weaver~` is the `@dynamic_gain` attribute. When enabled, `@dynamic_gain` automatically calculates and applies a dynamic gain multiplier to incoming audio bars based on a numerical rating property stored in the transcript dictionary for each bar.

This report explains the architecture, algorithm, and mathematical logic behind `@dynamic_gain`, compares how positive versus negative ratings are processed, and presents a technical breakdown of scenarios where a bar with a positive rating yields zero audio output despite receiving unity gain (`1.0`) from the dynamic gain system.

---

## Attribute Overview and Setup

### Definition and Default State

* **Attribute Name**: `dynamic_gain`
* **Type**: On/Off style integer (`0` = Disabled, `1` = Enabled)
* **Default Value**: `1` (Enabled)
* **Setter Method**: `weaver_attr_set_dynamic_gain`

When `@dynamic_gain 1` is set, `weaver~` reads the `rating` key associated with each bar in the transcript dictionary during playback. If `@dynamic_gain 0` is set, rating processing is bypassed, and all bars play at unity gain (`1.0`).

---

## Architectural Mechanics of Dynamic Gain

### 1. Rating Extraction from Transcript Dictionary

When a bar boundary is triggered in the DSP perform routine (`weaver_process_vector`), a task is scheduled on the queue thread (`weaver_audio_qtask`) to inspect the active transcript dictionary (`x->audio_dict_name`).

For a given track ID and bar timestamp:
1. The object retrieves the sub-dictionary corresponding to that bar.
2. It looks up the `rating` key (handling both single atom values and atom arrays).
3. If no `rating` key exists, the rating defaults to `1.0`.
4. The retrieved value is stored in `tr->pending_rating`.

### 2. Rolling Window Rating History

To compute relative dynamic attenuation without requiring prior knowledge of the entire song structure, `weaver~` maintains a thread-safe rolling rating buffer:

* **Ring Buffer**: `x->rolling_ratings` holds up to `MAX_ROLLING_RATINGS` (`65536`) timestamped rating entries (`t_rating_entry`).
* **Time Horizon (`x->song_length`)**: Calculated across all tracks as the maximum track duration in milliseconds.
* **Expiration (`weaver_expire_ratings`)**: Before processing a new bar rating at time `vector_time`, rating entries older than `vector_time - x->song_length` are evicted from the ring buffer.
* **Minimum Calculation (`weaver_get_rolling_min_rating`)**: Scans the active rolling window and returns the lowest rating value (`min_rating`).

### 3. Gain Calculation Formula

Inside `weaver_process_vector`, when new bar metadata is handed over to the DSP thread, the target gain is determined for the incoming crossfade slot (`other = 1 - active`):

```
target_gain = 1.0

if (x->dynamic_gain) {
    weaver_expire_ratings(x, vector_time);
    weaver_add_rating(x, vector_time, tr->pending_rating);
    min_rating = weaver_get_rolling_min_rating(x);

    if (tr->pending_rating < 0.0) {
        if (min_rating < 0.0) {
            target_gain = 1.0 - (tr->pending_rating / min_rating);
        }
    }
}

tr->gain[other] = target_gain;
```

---

## Positive vs. Negative Rating Processing

The core distinction in how `@dynamic_gain` evaluates ratings lies in whether the rating value is positive (`>= 0.0`) or negative (`< 0.0`).

### Positive Ratings (`pending_rating >= 0.0`)

1. **Unity Gain Assignment**: Any rating greater than or equal to zero (including missing ratings, `0.0`, `0.5`, `1.0`, or `10.0`) bypasses attenuation logic.
2. **Evaluation**: The condition `if (tr->pending_rating < 0.0)` evaluates to false.
3. **Result**: `target_gain` remains strictly `1.0`.
4. **Behavioral Context**: Positive ratings indicate acceptable or high-quality audio segments. They receive full unattenuated signal volume (`1.0` linear multiplier).

### Negative Ratings (`pending_rating < 0.0`)

1. **Dynamic Scaling**: Negative ratings signify undesirable, noisy, or low-quality bar slices.
2. **Evaluation**: The condition `if (tr->pending_rating < 0.0)` evaluates to true.
3. **Formula**:
   ```
   target_gain = 1.0 - (tr->pending_rating / min_rating)
   ```
4. **Mathematical Properties**:
   * Since both `tr->pending_rating` and `min_rating` are negative numbers, the division `tr->pending_rating / min_rating` yields a positive ratio between `0.0` and `1.0`.
   * **Worst-case rating in window**: If `tr->pending_rating` equals `min_rating` (the most negative rating seen within the last `song_length` duration), the ratio is `1.0`. `target_gain = 1.0 - 1.0 = 0.0` (complete attenuation / silence).
   * **Mildly negative rating**: If `min_rating = -10.0` and `tr->pending_rating = -2.0`, the ratio is `0.2`. `target_gain = 1.0 - 0.2 = 0.8` (`80%` linear volume).
   * **Near-zero negative rating**: As `tr->pending_rating` approaches `0.0` from the negative side, the ratio approaches `0.0`, and `target_gain` approaches `1.0`.
5. **Result**: Audio bars with negative ratings are smooth-scaled between complete silence (`0.0` gain) and full volume (`1.0` gain) relative to the worst rating in the recent timeline window.

---

## Audio Rendering and Crossfading Integration

Each track in `weaver~` contains two alternating source slots (`tr->gain[0]` and `tr->gain[1]`) managed by an internal crossfade engine (`t_crossfade_state`).

When a new bar is triggered:
1. The target gain (`1.0` for positive ratings, or scaled `0.0` to `1.0` for negative ratings) is assigned to the inactive slot.
2. The crossfade engine initiates an equal-power or linear ramp between slot 0 and slot 1 over `low_ms` to `high_ms` milliseconds.
3. During sample synthesis, the interpolated source samples are multiplied by their slot's gain:
   ```
   mix1 = interpolated_sample_slot_0 * ramp_factor_1 * tr->gain[0]
   mix2 = interpolated_sample_slot_1 * ramp_factor_2 * tr->gain[1]
   output_sample = mix1 + mix2
   ```

---

## State Resets and Boundary Behavior

To prevent old negative ratings from permanently suppressing audio after playback resets or wraps around:

* **Song Loop / Ramp Reset**: When `main_looped` is detected (input ramp wraps back to zero), `x->rolling_head` and `x->rolling_tail` are reset to `0`, clearing all historical rating entries.
* **`clear` Message**: Resets `x->rolling_ratings`, `x->lowest_rating_seen`, and sets all track slot gains `tr->gain[0]` and `tr->gain[1]` back to `1.0`.
* **`consolidate` Process**: Clears crossfade states and resets track lengths prior to running offline render passes.

---

## Speculative Analysis: How a Positive Rating May Yield Zero Volume

Even though a bar with a positive rating (`pending_rating >= 0.0`) is assigned a unity gain multiplier (`target_gain = 1.0`) by `@dynamic_gain`, several technical and architectural conditions within `weaver~` can result in complete silence (`0.0` final audio output) for that bar.

Below is a detailed breakdown of potential causes:

### 1. Missing or Unbound Source Palette Buffer

* **Missing Palette Key**: If the transcript specifies a palette symbol (e.g., `palette_99.wav`) that cannot be resolved in Max's buffer registry and the fallback stem buffer (`stems.<track_id>`) is also missing, `weaver~` sets the active palette symbol to `-` (silence).
* **Null Pointer Check**: In `weaver_process_vector`, if `tr->palette[j]` is set to `-` or `_sym_nothing`, buffer lookup is skipped. `tb[t].samples_src[j]` remains `NULL`, producing zero source samples (`interleaved_s = 0.0`).

### 2. Silent Source Audio Data

* **Zeroed Samples**: The source WAV buffer itself may contain zero-valued samples (e.g., recorded silence or muted sections). Multiplying zero-valued source samples by `tr->gain = 1.0` yields `0.0`.

### 3. Out-of-Bounds Palette Buffer Offset

* **Offset Beyond Frame Count**: If `offset` in the transcript points beyond the total duration of the palette buffer, the calculated frame index `f_src_raw = (offset + elapsed) * samplerate / 1000` exceeds `n_frames_src`.
* **Bounds Interlock**: The sample lookup requires `f_low >= 0 && f_high < tb[t].n_frames_src[j]`. When this condition fails, `interleaved_s` remains initialized to `0.0`, resulting in total silence.

### 4. Crossfade Ramp State and Fade Factors (`f1` / `f2`)

* **Fade-out Target**: When transitioning between slots, the crossfade engine calculates ramp factors `f1` and `f2` via `ramp_process()`. If slot 0 is fading out while slot 1 fades in, `f1` reaches `0.0`. If slot 1 fails to trigger properly, both `f1` and `f2` can be `0.0`.
* **Silence Slope Detector**: If the input signal amplitude was zero during the initial ramp phase, `ramp_process()` length clamping or toggle state may hold the fade multiplier at `0.0`.

### 5. Inactive or Zero-Length Track (`track_length <= 0.0`)

* **Track Bypassed**: If a track has a length of `0.0` ms (e.g., before dictionary consolidation or track length configuration), `weaver_process_vector` skips DSP calculations for that track (`continue`), writing no samples into the destination buffer.

### 6. Missing Destination Buffer (`polybuffer~`)

* **Unbound Output Buffer**: If the destination buffer (`poly_prefix.<track_id>`) is not bound or fails to allocate memory, `tb[t].samples_dest` is `NULL`. No audio writes occur.

### 7. Stopped or Static Control Sync Ramp (`ramp_in`)

* **Stationary Position**: `weaver~` synthesizes audio frames sequentially by tracking changes in `ramp_in` (inlet 1). If `ramp_in` is static (e.g., transport paused), `f_curr` equals `last_f_dest`. The sample processing loop does not execute, generating zero new audio frames.

### 8. Channel Mapping and Channel Count Mismatches

* **Zero Channels**: If the source palette buffer or destination buffer reports `0` channels, the channel loop limit (`n_chans`) evaluates to zero, bypassing sample assignment.

---

## Summary Matrix

| Rating Value | `@dynamic_gain` State | Dynamic Gain Applied (`target_gain`) | Output Volume (Assuming valid buffers & ramp) |
| :--- | :--- | :--- | :--- |
| **`>= 0.0` (Positive / Missing)** | Enabled (`1`) | `1.0` (Unity) | Full Signal Volume (`100%`) |
| **`< 0.0` (Worst in window)** | Enabled (`1`) | `0.0` (Silent) | Silent (`0%`) |
| **`< 0.0` (Intermediate)** | Enabled (`1`) | `1.0 - (rating / min_rating)` | Scaled Volume (`0%` to `100%`) |
| **Any Rating** | Disabled (`0`) | `1.0` (Unity) | Full Signal Volume (`100%`) |

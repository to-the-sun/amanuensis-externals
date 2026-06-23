# Detailed Report: `weaver~` Loop and Crossfade Behavior

This report details the internal logic and state management of the `weaver~` object, specifically focusing on its response to ramp loops, busy state transitions, and the dual-slot crossfade mechanism.

## 1. Ramp Loop Detection

The `weaver~` object monitors the input signal ramp (representing global "Song Time") to detect discontinuities. It distinguishes between two types of loops:

*   **Global Loop (`main_looped`):** Detected when the current input sample is less than the previous input sample. This signifies the master transport has looped or jumped backwards.
*   **Track-Specific Loop (`track_looped`):** Each track has a `track_length`. The object calculates a local position using `fmod(current_scan, tr->track_length)`. A loop is detected if this local scan value decreases.

### Impact of a Loop
When a loop is detected, several things happen:
1.  **FIFO Event:** A `TYPE_LOOP` entry is added to the internal FIFO.
2.  **Outlet Output:** The main thread (via `weaver_audio_qtask`) outputs the Track ID from the **Loop Outlet** (Outlet 1).
3.  **Visualization Reset:** If `main_looped` is true and visualization is enabled, a `{"clear": 1}` command is sent to the visualizer to reset the track graph.
4.  **Temporal Continuity:** To prevent the "gap-filling" logic from attempting to interpolate across the loop boundary (which would result in thousands of garbage samples), the `tr->last_f_dest` tracker is reset.

## 2. Busy State Lifecycle

The `busy` state of a track is controlled by two primary flags: `tr->busy` and `tr->waiting_for_dict`.

### Lifecycle Stages:
1.  **Idle (`busy=0`, `waiting_for_dict=0`):** The track is either silent or playing a stable source in one of its slots.
2.  **Triggered (`busy=1`, `waiting_for_dict=1`):** A bar hit is detected in the DSP thread. The track enters a "busy" state and waits for the main thread to resolve the bar's metadata (palette and offset) from the transcript dictionary.
3.  **Metadata Handover:** Once the main thread finds the dictionary data, it sets `tr->has_pending_data` and clears `tr->waiting_for_dict`.
4.  **Transitioning:** The DSP thread detects `has_pending_data`, determines if a crossfade is needed, and updates the "other" slot.
5.  **Fading:** The `ramp_process` function (from `shared/crossfade.c`) gradually moves the volume of the old slot to 0.0 and the new slot to 1.0.
6.  **Resolution:** The track remains `busy` until **both** ramps have reached their targets (0.0 or 1.0) AND `waiting_for_dict` is 0. Only then is `tr->busy` set back to 0.

## 3. Dual-Slot Crossfade Mechanism

`weaver~` uses a dual-slot architecture to allow seamless transitions between different audio sources (palettes) or different temporal offsets within the same source.

*   **Slots:** `tr->palette[2]` and `tr->offset[2]`.
*   **Control:** `tr->control` is a float that acts as a toggle (0.0 or 1.0). It determines which slot is "Target A" and which is "Target B" for the crossfade.

### The Handover Logic
When new metadata arrives:
1.  It compares the `pending_palette` and `pending_offset` with the *currently active* slot.
2.  If a change is detected (or if a "force" symbol like `0` is used), it swaps the `control` value.
3.  It sets `tr->xf.direction` to `new_control - old_control` (e.g., `1.0 - 0.0 = 1.0`).
4.  The "other" slot is populated with the new palette and offset.

### Ramp Processing
In the DSP loop, `ramp_process` is called for both slots:
- **Slot 1:** Moves toward `1.0` if `direction > 0`, or `0.0` if `direction < 0`.
- **Slot 2:** Moves in the opposite direction (`direction * -1.0`).

The `ramp_process` is "smart": it uses a sliding amplitude follower (`x->last_amp`) and a target length (`high_ms`) to ensure that crossfades are fast during silence but smooth during active audio, preventing clicks.

## 4. Palette and Offset Temporal Anchoring

A critical aspect of the "weaving" process is how source audio is mapped to the destination.

### The Offset Calculation
When metadata is handed over:
`tr->offset[other] = tr->pending_offset - tr->viz_ms;`

*   `tr->pending_offset`: The absolute millisecond position within the source `palette` buffer (from the dictionary).
*   `tr->viz_ms`: The "Song Time" (ramp value) at the exact moment the bar hit was detected.

By storing the *difference*, the DSP thread can calculate the correct source sample for **any** future song time `v_at_f`:
`src_ms = tr->offset[j] + v_at_f;`

This "anchoring" ensures that even if the ramp jumps or fluctuates, the audio source stays perfectly locked to the intended rhythmic position.

## 5. Summary of Loop Interaction

When the input ramp loops:
1.  **DSP Detects:** `main_looped = true`.
2.  **DSP Updates:** `last_f_dest` is reset. The `elapsed` timer continues but the interpolation gap is bypassed.
3.  **FIFO:** A `TYPE_LOOP` event is queued.
4.  **Bar Retrigger:** Because a loop is essentially a large jump, the "Initial Bar Trigger" logic (or the `main_looped` check in the continuous detection) fires a new bar hit for the beginning of the loop.
5.  **Handover:** The dictionary lookup is performed again for the start of the song/loop, potentially triggering a crossfade if the palette or offset at the start differs from what was playing at the end.

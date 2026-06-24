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

The `busy` state of a track is a compound state controlled by two primary flags: `tr->busy` (indicating a transition or pending action) and `tr->waiting_for_dict` (indicating the track is blocked waiting for metadata from the main thread).

### Lifecycle Stages:
1.  **Idle (`busy=0`, `waiting_for_dict=0`):** The track is either silent or playing a stable source in one of its slots.
2.  **Triggered (`busy=1`, `waiting_for_dict=1`):** A bar hit is detected in the DSP thread. The track enters a "busy" state and waits for the main thread to resolve the bar's metadata (palette and offset) from the transcript dictionary.
3.  **Metadata Handover:** Once the main thread finds the dictionary data, it sets `tr->has_pending_data` and clears `tr->waiting_for_dict`.
4.  **Transitioning:** The DSP thread detects `has_pending_data`, determines if a crossfade is needed, and updates the "other" slot.
5.  **Fading:** The `ramp_process` function (from `shared/crossfade.c`) gradually moves the volume of the old slot to 0.0 and the new slot to 1.0.
6.  **Resolution:** The track remains `busy` until the following three conditions are simultaneously met:
    - **Ramp 1 is Done:** Target amplitude (0.0 or 1.0) reached.
    - **Ramp 2 is Done:** Target amplitude (0.0 or 1.0) reached.
    - **Metadata Processed:** `tr->waiting_for_dict` is 0.
    Only when all three are true is `tr->busy` set back to 0.

### The Clean Slate Logic (Global Transport Reset)
To ensure arbitrary loop points trigger correctly and stale metadata is elegantly discarded, the system implements a "Clean Slate" reset specifically for global transport jumps (`main_looped`).

**Implementation:** Upon `main_looped`, the system performs an atomic reset of the entire state pipeline:
1.  **Force Ramps to Done:** Both crossfade ramps are snapped to their current targets by updating their internal `go` time.
2.  **Clear Track Pipeline Flags:** `tr->waiting_for_dict`, `tr->has_pending_data`, and `tr->busy` are all reset to 0.
3.  **Drain the FIFO:** The global bar hit FIFO is cleared (`x->fifo_head = x->fifo_tail`). This effectively cancels any metadata lookup requests that have been queued in the DSP thread but not yet processed by the main thread.

#### Effectiveness and Elegance
*   **Stale Metadata Mitigation:** Draining the FIFO is the primary defense against race conditions. Since the FIFO is the only communication channel for dictionary lookups, clearing it ensures the main thread perform no further stale lookups for the previous cycle.
*   **Atomic Reset:** By clearing all flags and snapping ramps simultaneously, the track becomes truly idle at the exact sample of the transport jump. This allows the track to immediately (in the same DSP vector) detect and trigger the *correct* bar key for the new transport position.
*   **Arbitrary Loop Compatibility:** This architecture is agnostic to bar boundaries. It doesn't force a search to Bar 0; instead, it readies the track to search for *any* bar relevant to the destination time.
*   **Sonics:** Snapping ramps at a transport jump is sonically transparent as the jump itself is a massive audio discontinuity.

#### Lifecycle of `main_looped`
The `main_looped` flag is a local boolean within the DSP loop. It is set to `true` for **exactly one sample**—the sample where the incoming ramp value is detected to be less than the previous sample. As soon as the ramp resumes its upward movement on the next sample, the comparison `current_scan < last_scan` becomes false, and `main_looped` returns to `false`.

#### Potential for Missed Bar Triggers
In the current implementation, there is a technical possibility that a reset could be missed if the loop jump lands in a very specific way:
1.  **Millisecond Quantization:** The logic currently requires `r_scan != r_last` (a change in the integer millisecond value) to trigger a bar check. If a loop jump occurs but the destination time `current_scan` falls within the same integer millisecond as the source time `last_scan` (e.g., jumping from 1000.9ms to 1000.1ms), the `r_scan != r_last` check will fail and the trigger will be skipped.
2.  **Global Reset vs. Local Loop:** Currently, `main_looped` (global) triggers the Clean Slate reset to ensure re-triggering, while `track_looped` (local) does not. This is intentional as `track_looped` events are part of the steady-state performance of a track and do not signify a fundamental transport shift that requires a state reset.

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

## 6. Proposed Enhancements for Clean Slate Completeness

While the current Clean Slate logic addresses the immediate stability of the state machine, further refinements could ensure a truly seamless "reset-to-zero" experience:

### Eliminating Stale Audio Crossfades
One remaining artifact is that if a loop occurs while audio is playing, the system may attempt to crossfade from the *previous* bar's audio to the *new* bar's audio at the loop destination. Because the transport has jumped, this crossfade is often musically irrelevant.
- **Speculation:** Resetting the active `palette` and `offset` slots (or clearing the `control` index history) upon `main_looped` could force the next bar hit to perform a "hard jump" rather than a crossfade. This would ensure that the very first sound heard at the start of the loop is exclusively from the new source.

### Temporal State Reset
The `last_track_scan` variable tracks the local position of a track.
- **Speculation:** Resetting `tr->last_track_scan` to `-1.0` upon `main_looped` would force the track to re-enter the "Initial Bar Trigger" logic. This ensures that the detection for the new transport position is handled with the same "cold start" reliability as when the object is first initialized, potentially avoiding redundant `TYPE_LOOP` events or complex range calculations in the same vector as the loop.

### Visualization and Timer Synchronization
- **Speculation:** The `tr->xf.elapsed` timer and visualization flags (like `viz_trigger_dirty` and `viz_dirty`) could be explicitly reset. While the `{"clear": 1}` command resets the UI, ensuring the internal visualization state is synchronized prevents the delivery of "stale" visual updates from the previous song cycle immediately after the loop. Similarly, resetting the `elapsed` timer to match the new `f_curr` would provide a consistent temporal baseline for the new cycle's crossfade ramps.

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

### The Refined Loop Override Proposal (Strictly for `main_looped`)
A more robust solution is proposed for global song resets to ensure arbitrary loop points trigger correctly and stale metadata is elegantly discarded.

**Refined Proposal:** Upon `main_looped` (global transport jump), the system should perform an atomic "Clean Slate" reset of the entire state pipeline:
1.  **Force Ramps to Done:** Snap both crossfade ramps to their current targets.
2.  **Clear Track Pipeline Flags:** Set `tr->waiting_for_dict = 0`, `tr->has_pending_data = 0`, and `tr->busy = 0`.
3.  **Drain the FIFO:** Set `x->fifo_head = x->fifo_tail`. This effectively "kills" any metadata lookup requests that have been queued in the DSP thread but not yet processed by the main thread.

#### Speculation and Critique of the Proposal
*   **Elegance of FIFO Draining:** This is a highly elegant way to handle stale lookups. Since the FIFO is the only communication channel from the DSP thread to the main thread's dictionary lookup logic, clearing it instantly invalidates any "in-flight" requests from the previous transport cycle. The main thread will find an empty queue and perform no further stale lookups.
*   **Atomic "Clean Slate":** By clearing `has_pending_data` along with the FIFO, we ensure that even if the main thread *just* finished a lookup and set the flag, the DSP thread will ignore it. The track becomes truly idle at the exact sample of the transport jump.
*   **Arbitrary Loop Compatibility:** This reset allows the track to immediately (in the same DSP vector) detect and trigger the *correct* bar key for the new transport position.
*   **Race Condition Mitigation:** Because `main_looped` is a global event detected by the DSP thread, and the FIFO is managed with a simple head/tail index, draining it is thread-safe as long as the indices are updated atomically. This completely removes the concern about the main thread overwriting new state with stale metadata.
*   **Sonics:** Snapping ramps at a transport jump is sonically safe as the jump itself is a massive discontinuity.
*   **Conclusion:** This refined approach provides a total synchronization reset. It ensures that the very first bar detected after a loop/jump is processed by a completely idle track with no "memory" of the previous cycle.

#### Lifecycle of `main_looped`
The `main_looped` flag is a local boolean within the DSP loop. It is set to `true` for **exactly one sample**—the sample where the incoming ramp value is detected to be less than the previous sample. As soon as the ramp resumes its upward movement on the next sample, the comparison `current_scan < last_scan` becomes false, and `main_looped` returns to `false`.

#### Potential for Missed Bar Triggers
In the current implementation, there is a technical possibility that a reset could be missed if the loop jump lands in a very specific way:
1.  **Millisecond Quantization:** The logic currently requires `r_scan != r_last` (a change in the integer millisecond value) to trigger a bar check. If a loop jump occurs but the destination time `current_scan` falls within the same integer millisecond as the source time `last_scan` (e.g., jumping from 1000.9ms to 1000.1ms), the `r_scan != r_last` check will fail and the trigger will be skipped.
2.  **Global Reset vs. Local Loop:** Currently, `main_looped` (global) provides a `busy` bypass to ensure re-triggering, while `track_looped` (local) does not. This is intentional as `track_looped` events are part of the steady-state performance of a track and do not signify a fundamental transport shift that requires a state reset.

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

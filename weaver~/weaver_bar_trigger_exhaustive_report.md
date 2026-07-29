# Complete Technical Report: Step-by-Step Lifecycle of a Bar Trigger in `weaver~`

This exhaustive report details the internal mechanics, state management, and mathematical principles of the `weaver~` Max MSP external. It tracks every sequence of events triggered by a bar hit from the millisecond-level detection in the DSP thread to dictionary resolution on the Main thread, dual-slot crossfade blending, fallback routines, looping mechanics, and a side-by-side comparative analysis of operations in positive versus negative time.

---

## 1. Architectural Overview

The `weaver~` object performs real-time and offline "audio weaving." It reads a global ramp signal representing master song time, matches that time to track-specific grids, queries a transcript JSON dictionary for arrangement directions (palettes and offsets), and dynamically crossfades between source buffers to output seamless audio channels.

### Core Structures
1. **`t_weaver`**: The parent Max object. It holds global state, such as `most_negative_bar`, `dynamic_gain` toggle, and references to the `bar` buffer and global transcripts.
2. **`t_weaver_track`**: State cache representing a single track. It tracks busy state, dual-slot properties, buffers, gain, and local scanning variables.
3. **`t_fifo_entry` / `hit_bars` FIFO**: A 4096-entry ring buffer used to pass bar triggers from the lock-free audio DSP thread to the Main thread.
4. **Dual Slots (`j = 0` or `1`)**: Two independent sample playheads (`palette` and `offset` state) maintained per track to enable seamless overlap and crossfades.

---

## 2. Step-by-Step Lifecycle of a Bar Trigger Event

The lifecycle of a single bar trigger unfolds across seven distinct phases as described below.

```
+-------------------------------------------------------+
|                DSP Thread (Phase 1)                   |
| - Ramp scan, fmod(current_scan, track_length)         |
| - Detect boundary crossing (r_scan != r_last)        |
| - Push TYPE_DATA to FIFO; set busy=1, wait_dict=1     |
+-------------------------------------------------------+
                           | (FIFO Ring Buffer)
                           v
+-------------------------------------------------------+
|               Main Thread (Phase 2 & 3)               |
| - qelem triggers weaver_audio_qtask                   |
| - Read FIFO; parse dictionary for track & bar keys    |
| - Extract palette, offset, rating symbols             |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|               Main Thread (Phase 4)                   |
| - Resolve palette buffers (bind & kick)               |
| - Fall back to stems.[track_id] or silence "-" if missing |
| - Handover via weaver_update_track_metadata           |
+-------------------------------------------------------+
                           | (Lock-free Handover)
                           v
+-------------------------------------------------------+
|                DSP Thread (Phase 5)                   |
| - Detect has_pending_data; swap slot (active vs other)|
| - Compute offset anchor & dynamic gain                |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|                DSP Thread (Phase 6)                   |
| - Fill gap [last_f_dest + 1, f_curr]                  |
| - Linearly interpolate source samples                 |
| - Process crossfade ramps (sliding followers)         |
+-------------------------------------------------------+
                           |
                           v
+-------------------------------------------------------+
|                DSP Thread (Phase 7)                   |
| - Ramps finish; clear busy state                      |
| - Broadcast TCP visualizer packets                    |
+-------------------------------------------------------+
```

---

### Phase 1: Bar Boundary Crossing Detection (DSP Thread)

During each vector processing pass, `weaver_process_vector` calculates the absolute time position for each sample frame $i$ inside the block:
$$\text{current\_scan} = \text{ramp\_in}[i] + x\rightarrow\text{most\_negative\_bar}$$

For each active track, the local scan position mapping is derived:
$$\text{tr\_scan} = fmod(\text{current\_scan\_for\_track}, tr\rightarrow\text{track\_length})$$

#### Boundary Comparison Math:
The DSP thread quantizes both the current and the previous scan positions to the nearest floored millisecond:
$$\text{r\_scan} = \lfloor \text{tr\_scan} \rfloor$$
$$\text{r\_last} = \lfloor tr\rightarrow\text{last\_track\_scan} \rfloor$$

If $tr\rightarrow\text{last\_track\_scan} \neq -1.0$, a transition check is evaluated:
$$\text{r\_scan} \neq \text{r\_last}$$

If a boundary has been crossed, the continuous triggering planning module determines the target bar interval $j_{\text{latest}}$:
$$\text{start} = (\text{track\_looped} \ || \ \text{main\_looped}) \ ? \ 0 \ : \ \text{r\_last} + 1$$
$$\text{end} = \text{r\_scan}$$
$$j_{\text{latest}} = \lfloor \frac{\text{end}}{\text{bar\_len}} \rfloor \times \text{bar\_len}$$

If $j_{\text{latest}} \ge \text{start}$, a valid boundary cross is confirmed:
1. A new event of type `TYPE_DATA` is pushed onto the global lock-free ring buffer `hit_bars[x->fifo_tail]`.
2. The event records the quantized relative trigger time: `hit_bars[tail].rel_time = (double)latest_j`.
3. The event records the current absolute time anchor: `hit_bars[tail].bar.value = current_scan`.
4. The FIFO tail index is advanced safely using modulo arithmetic:
   $$\text{tail}_{\text{new}} = (\text{tail} + 1) \pmod{4096}$$
5. The track immediately enters its waiting state:
   - $tr\rightarrow\text{waiting\_for\_dict} = 1$
   - $tr\rightarrow\text{busy} = 1$

---

### Phase 2: Task Scheduling (Main Thread)

As soon as a thread completes its DSP loop, a low-priority queue scheduler `qelem` is flagged:
```c
qelem_set(x->audio_qelem);
```
The operating system scheduler wakes up the Max Main (Message) Thread to execute `weaver_audio_qtask`. This thread:
1. Drains and processes internal log messages and asynchronous buffer flags under lock.
2. Traverses and processes all enqueued FIFO events starting from `fifo_head` until `fifo_head == fifo_tail`.
3. Safely extracts the track ID (`hit_entry.track_id`) and absolute/relative trigger timestamps.

---

### Phase 3: Dictionary Lookup & Metadata Resolution (Main Thread)

Using the parent object's registered transcript name, the task performs a series of lookups within the JSON schema:
1. **Find Registered Transcript**: Resolves the root dictionary object via `dictobj_findregistered_retain(x->audio_dict_name)`.
2. **Track Partitioning**: Checks for a sub-dictionary corresponding to the current track ID string (e.g., `"1"`).
3. **Bar Timestamp Retrieval**: Converts the relative millisecond value `rel_time` into a string key (e.g., `"4000"`) and attempts to locate the bar's internal dictionary.
4. **Data Extraction & Atom Coercion**:
   - Extraction is designed to be highly robust; it supports reading values both as raw atoms and single-element atom arrays (addressing different JSON serialization structures).
   - Extracts `palette` (symbol), `offset` (float, in ms), and `rating` (float).

---

### Phase 4: Fallback Resolution Sequence (Main Thread)

If the dictionary lookup succeeds, the object verifies that the requested `palette` buffer is loaded in the Max environment. If it is not, a fallback cascade is initiated:

```
                  [Requested Palette]
                           |
                 Is Palette Bound in Max?
                 /                     \
             (Yes)                     (No)
              /                           \
       Use Palette Buffer          [Fallback to stems]
                                    Is stems.[track_id] Bound?
                                    /                     \
                                (Yes)                     (No)
                                 /                           \
                         Use stems Buffer             [Fallback to Silence]
                         Offset = Absolute Time       Use "-" (Silence)
                                                      Offset = 0.0
```

#### Step 1: Active Buffer Ref Kick (Palette Verification)
A short-lived `t_buffer_ref` is constructed. If it fails to locate the buffer object, `weaver~` executes a "kick" (clearing the binding to `_sym_nothing` and rebinding directly) to force Max to update its search path:
```c
buffer_ref_set(temp_ref, _sym_nothing);
buffer_ref_set(temp_ref, palette);
```
If the buffer is resolved, `palette_exists` is flagged `1`.

#### Step 2: Fallback 1 — `stems.[track_id]`
If the palette is missing, undefined, or explicitly set to `"-"` (silence), the track attempts to bind to its default stems lane:
- **Identifier**: `stems.[track_id]` (e.g., `stems.2`).
- **Kick Routine**: The object executes a kick on a temporary `stems` buffer reference to resolve the underlying buffer object.
- **Playback Alignment (Offset Mapping)**: If the stems buffer is resolved, the offset is set to:
  $$\text{offset} = \text{hit.value} - x\rightarrow\text{most\_negative\_bar}$$
  This maps the relative playback position perfectly to the positive-indexed stems buffer based on where the trigger landed in the timeline.

#### Step 3: Fallback 2 — Silence (`"-"`)
If the stems buffer is also missing, or if the bar key was completely absent from the track dictionary in the first place:
- **Palette**: `"-"` (silence symbol).
- **Offset**: `0.0`.
- This ensures the track smoothly fades to silence rather than hanging, outputting digital noise, or crashing.

Once the metadata is resolved, the task invokes `weaver_update_track_metadata` which copies these values into the track's thread-safe pending structure under a mutex lock:
- $tr\rightarrow\text{pending\_palette} = \text{palette}$
- $tr\rightarrow\text{pending\_offset} = \text{offset}$
- $tr\rightarrow\text{has\_pending\_data} = 1$

---

### Phase 5: DSP Metadata Handover & Slot Swapping (DSP Thread)

At the start of the next audio vector processing step, the DSP thread checks if `tr->has_pending_data` is active.

#### 1. Change Detection
The DSP thread compares the pending data against the *currently active slot* (indexed by `active = (int)round(tr->control)`):
$$\text{change} = (tr\rightarrow\text{pending\_palette} \neq tr\rightarrow\text{palette}[\text{active}] \ || \ tr\rightarrow\text{pending\_offset} \neq tr\rightarrow\text{dict\_offset}[\text{active}] \ || \ tr\rightarrow\text{pending\_bar\_symbol} == \text{\_sym\_0})$$

#### 2. Slot Swapping and Anchor Calculation
If a change is detected, the track swaps playback slots to target the inactive slot:
$$\text{other} = 1 - \text{active}$$

The playback offset for the other slot is calculated as:
$$\text{tr}\rightarrow\text{offset}[\text{other}] = tr\rightarrow\text{pending\_offset} - tr\rightarrow\text{viz\_ms}$$

By subtracting the absolute trigger timestamp `viz_ms` from the target offset, the DSP thread anchors the source audio playback relative to the song timeline. The source playhead index for any future frame time $v_{\text{at\_f}}$ is computed as:
$$\text{src\_ms} = tr\rightarrow\text{offset}[\text{other}] + v_{\text{at\_f}}$$

#### 3. Dynamic Gain Adjustment
If the attribute `@dynamic_gain` is active, the track's target gain is adjusted. Using a strictly monotonic vector timestamp, the track's rating is added to a rolling circular buffer (capacity `65536` elements). The rolling minimum rating seen within the last `song_length` window is determined:
- If the bar rating is positive or missing, $\text{gain} = 1.0$.
- If the rating is negative, the gain is scaled:
  $$\text{gain} = 1.0 - \left( \frac{\text{rating}}{\text{rolling\_min\_rating}} \right)$$

This forces highly negative (poorly rated) bars to be attenuated proportionally to the worst-rated segment in the active rolling window.

---

### Phase 6: Audio Synthesis & Dual-Slot Crossfading (DSP Thread)

`weaver~` iterates through each destination sample frame between the last processed destination sample and the current vector position:
$$f \in [tr\rightarrow\text{last\_f\_dest} + 1, f_{\text{curr}}]$$

This **gap-filling** loop is mathematically necessary to prevent temporal aliasing and audio crackles during rapid transport jumps or high-speed ramp fluctuations.

#### 1. Sub-Sample Accurate Linear Interpolation
For both slot playheads $j \in \{0, 1\}$, if the source buffer is active:
1. Calculates the raw fractional source sample:
   $$f_{\text{src\_raw}} = \frac{\text{src\_ms} \times \text{samplerate}_{\text{src}}}{1000.0}$$
2. Floors the fractional position: $f_{\text{low}} = \lfloor f_{\text{src\_raw}} \rfloor$, $f_{\text{high}} = f_{\text{low}} + 1$.
3. Calculates the sub-sample interpolation fraction: $\text{frac} = f_{\text{src\_raw}} - f_{\text{low}}$.
4. Computes the blended output sample:
   $$s_{\text{interp}} = s_{\text{low}} + (s_{\text{high}} - s_{\text{low}}) \times \text{frac}$$
5. Evaluates and tracks peak amplitude levels `max_abs[j]` across channels to feed the sliding envelope followers.

#### 2. Sliding Envelope Follower & Dual Ramps (`ramp_process`)
The crossfade engine handles two independent ramps (`ramp1` and `ramp2`) using a sliding envelope follower to prevent clicks by ensuring transitions are rapid during silence but smooth during active audio signals:

```
                   [Incoming Abs Signal]
                             |
                     Is Abs > last_amp?
                     /                \
                 (Yes)                (No)
                  /                      \
          amp = Abs (Immediate)      amp = Slide Down (low_ms)
                  \                      /
                   \                    /
                    [Target Ramp Length]
                length = amp * high_samples (Clipped)
                             |
                      [Evaluate Fade]
                   fade = age / length (Clipped 0.0-1.0)
                   fade = |toggle - fade|
```

- **Ramp 1** processes slot 0 toward $1.0$ (if direction $> 0$) or $0.0$ (if direction $< 0$).
- **Ramp 2** processes slot 1 in the exact opposite direction.
- The direction value is reset to `0.0` immediately after the first frame step of a transition block is applied.

#### 3. Output Mixing
The final mixed output sample for each channel $c$ is computed:
$$\text{mix} = (\text{interleaved\_s}[0][c] \times f_1 \times tr\rightarrow\text{gain}[0]) + (\text{interleaved\_s}[1][c] \times f_2 \times tr\rightarrow\text{gain}[1])$$

---

### Phase 7: Transition Completion & Busy Signal Release

Once the gap-filling synthesis block completes, the DSP thread checks if both ramps have reached their destinations:
$$\text{r1\_done} = (tr\rightarrow\text{xf.ramp1.toggle} > 0.5) \ ? \ (f_1 \le 0.0) \ : \ (f_1 \ge 1.0)$$
$$\text{r2\_done} = (tr\rightarrow\text{xf.ramp2.toggle} > 0.5) \ ? \ (f_2 \le 0.0) \ : \ (f_2 \ge 1.0)$$

If both ramps have completed their transitions and the main thread is no longer searching the dictionary:
$$\text{r1\_done} \ \wedge \ \text{r2\_done} \ \wedge \ \neg tr\rightarrow\text{waiting\_for\_dict}$$

The track's busy signal is released:
- $tr\rightarrow\text{busy} = 0$

#### High-Frequency Visualizer Updates:
The DSP thread copies the active gains ($f1, f2$) and state data to a thread-safe visualization block. If a transition took place, or if more than 333.33 milliseconds have elapsed, the `viz_dirty` or `viz_trigger_dirty` flag is tripped. The main-thread queue task collects these updates outside of the critical lock and broadcasts them over the TCP connection to `debug_visualizer.py`.

---

## 3. Empty/Silent Bar Handling

When a bar is explicitly marked as silent in the transcript dictionary (e.g., using a hyphen `"-"` or an empty string `""` as the palette name), `weaver~` skips fallback lookups to ensure clean playback and performance:

1. **Suppression of Falls**: The main thread detects the silence symbol (`"-"` or `_sym_dash`) and directly bypasses the `stems.[track_id]` fallback routine.
2. **Buffer Release**: The track's active slot `palette` is updated to `"-"` and its buffer references are safely released.
3. **Seamless Transition**: The DSP thread initiates a standard crossfade towards the silence slot. The sliding envelope follower uses the decay properties configured by the `@low_ms` and `@high_ms` attributes, smoothly fading out the active audio channel to eliminate clicks.

---

## 4. Planning and Scheduling the Next Bar Trigger

To support continuous forward playback, `weaver~` plans and schedules its triggers by mapping the floored millisecond counters `r_scan` and `r_last`.

### The Boundary Math
Assume a bar length of $L = 2000\text{ ms}$:
- Let $r_{\text{last}} = 1999\text{ ms}$ (the last millisecond processed).
- Let $r_{\text{scan}} = 2000\text{ ms}$ (the current millisecond).
- Playback is moving forward, so $\text{track\_looped}$ is false.

#### Calculating Boundaries:
1. Identify the range:
   $$\text{start} = r_{\text{last}} + 1 = 2000$$
   $$\text{end} = r_{\text{scan}} = 2000$$
2. Calculate the latest bar boundary:
   $$j_{\text{latest}} = \lfloor \frac{\text{end}}{L} \rfloor \times L = \lfloor \frac{2000}{2000} \rfloor \times 2000 = 2000$$
3. Compare:
   $$j_{\text{latest}} \ge \text{start} \implies 2000 \ge 2000 \quad (\text{True})$$

**Outcome**: A bar trigger event is queued for $2000\text{ ms}$.

On the next millisecond step:
- $r_{\text{last}} = 2000\text{ ms}$
- $r_{\text{scan}} = 2001\text{ ms}$
- $\text{start} = 2001$, $\text{end} = 2001$.
- Calculation:
  $$j_{\text{latest}} = \lfloor \frac{2001}{2000} \rfloor \times 2000 = 2000$$
- Compare:
  $$j_{\text{latest}} \ge \text{start} \implies 2000 \ge 2001 \quad (\text{False})$$

**Outcome**: The comparison is false, meaning **exactly one trigger** is scheduled and fired per bar boundary crossing.

---

## 5. Looping Behaviors

`weaver~` manages two levels of looping to support continuous, stable audio playback.

### 5.1 Global Loop Reset (`main_looped`)

A global loop occurs when the absolute master ramp jumps backwards:
$$\text{current\_scan} < \text{last\_scan}$$

When a global loop is detected, `weaver~` executes a **"Clean Slate" reset** during that exact sample vector to prevent stale state from corrupting the new timeline pass:

1. **Clear FIFO Queue**: Safely clears the hit-bars FIFO (`fifo_head = fifo_tail`) to cancel pending dictionary queries from the previous cycle.
2. **Force Ramp Terminations**: Instantly snaps the active crossfade ramps to their target lengths:
   $$tr\rightarrow\text{xf.ramp1.go} = tr\rightarrow\text{xf.elapsed} - tr\rightarrow\text{xf.ramp1.length}$$
3. **Reset Slots**: Resets both slots to `"-"` (silence) and anchors to `-1.0` to force a hard jump on the next trigger rather than a slow crossfade.
4. **Trigger Loop Event**: Enqueues a `TYPE_LOOP` event to inform the Main thread to output a trigger on the loop outlet and send a `{"clear": 1}` packet to the visualizer.
5. **Re-align Timeline Playback**: Sets `last_track_scan` to `-1.0` to force initial boundary detection, and syncs `tr->xf.elapsed` with the destination sample index.

---

### 5.2 Individual Track Looping (Negative and Positive)

When a track is shorter than the master song duration, it loop-scans its own content bounds independently. This behavior differs based on whether the playback occurs in positive or negative time:

#### Positive Time Looping
$$\text{tr\_scan} = fmod(\text{current\_scan}, tr\rightarrow\text{track\_length})$$
This wraps the scan head forward continuously. A loop boundary is crossed when `r_scan < r_last`, which queues a track-specific `TYPE_LOOP` event.

#### Negative Time Looping
When the playback position moves below the track's most negative boundary:
$$\text{current\_scan} < tr\rightarrow\text{most\_negative\_bar}$$

The track loops backwards through its content bounds using a modular mapping function:
1. Calculates the track's total content span:
   $$T_{\text{span}} = tr\rightarrow\text{highest\_bar} - tr\rightarrow\text{most\_negative\_bar} + L$$
2. Derives the distance from the negative starting boundary:
   $$\text{diff} = tr\rightarrow\text{most\_negative\_bar} - \text{current\_scan}$$
3. Wraps the difference within the content span:
   $$\text{wrapped\_diff} = fmod(\text{diff}, T_{\text{span}})$$
4. Calculates the looped scan position:
   - If $\text{wrapped\_diff} = 0.0$:
     $$\text{current\_scan\_for\_track} = tr\rightarrow\text{most\_negative\_bar}$$
   - Otherwise:
     $$\text{current\_scan\_for\_track} = tr\rightarrow\text{highest\_bar} - (\text{wrapped\_diff} - L)$$

This maps the reverse-running timeline so that the track's internal playhead continues to scan forward at normal speed, playing the loop structure seamlessly.

---

## 6. Comparison: Below Zero vs. Above Zero Operations

The table below contrasts the internal execution paths of `weaver~` when running in positive versus negative time.

| Architectural Component | Above Zero ($t \ge 0$) | Below Zero ($t < 0$) |
| :--- | :--- | :--- |
| **Mathematical Division Operator** | Forward division truncates toward zero, matching mathematical floor division. | Integer division `/` truncates toward zero, while floor division rounds down toward negative infinity. This difference was the root cause of negative-looping bugs. |
| **Continuous Trigger Planning Math** | For $t = 150\text{ ms}, L = 100\text{ ms}$: <br> $j_{\text{latest}} = \lfloor \frac{150}{100} \rfloor \times 100 = 100$. <br> $j_{\text{latest}} \ge \text{start} \implies 100 \ge 150$ is **False**. <br> **Fires exactly once** per boundary. | For $t = -150\text{ ms}, L = 100\text{ ms}$: <br> $j_{\text{latest}} = \text{floor}(-150 / 100) \times 100 = -200$. <br> $j_{\text{latest}} \ge \text{start} \implies -200 \ge -150$ is **False** (resolved via floor division). |
| **Trigger Frequency** | Undergoes steady-state gating; queues exactly one event at each boundary crossing. | Before the mathematical fix, the division mismatch caused $j_{\text{latest}} \ge \text{start}$ to remain True continuously, flooding the FIFO with redundant triggers on every millisecond step. |
| **TCP Socket & Visualizer Queue** | Transmits updates at stable intervals (e.g., every $333\text{ ms}$ or at boundary crossings). | Prior to the fix, the millisecond-level flood overloaded the TCP socket and quickly exhausted the visualizer's 1000-element queue, purging historical track data and causing visual "scrunching." |
| **Loop Boundary Triggers** | Loop events are triggered cleanly at loop boundaries. | Before the fix, negative trigger values (e.g., $j_{\text{latest}} = -100$) evaluated to less than the loop start value ($0$), completely skipping the loop's initial bar trigger. |
| **Individual Track Looping** | Standard modulo wrapping wraps the playback scan head forward. | Utilizes modular wrapping based on $T_{\text{span}}$ to map the reverse timeline, enabling the track's playhead to continue scanning forward at normal speed. |
| **Offset Anchor Calculation** | $\text{tr}\rightarrow\text{offset} = \text{pending\_offset} - \text{viz\_ms}$ matches positive absolute positions. | Offset anchor calculation is applied identically, mapping negative timestamps to positive buffer index space. |
| **Stems Fallback Playhead Alignment** | Fallback offset aligns directly with positive buffer indices. | Uses $\text{offset} = \text{hit.value} - x\rightarrow\text{most\_negative\_bar}$ to map negative timeline absolute positions to positive destination buffer frames. |

---

## 7. Key Takeaways & Design Lessons

1. **Floor Division in DSP Systems**: When writing DSP logic that spans negative boundaries, standard C/C++ integer division (`/`) cannot be used for wrapping or quantization. You must use mathematical floor division (e.g., `floor()` or dedicated rounding functions) to ensure consistent behavior across negative boundaries.
2. **Decoupled Architecture with Ring Buffers**: Passing events between the real-time audio thread and the message thread using a lock-free FIFO ring buffer ensures high-performance audio processing. The message thread can perform complex, low-priority tasks like dictionary lookups and socket writes without interrupting the audio stream.
3. **Strict State Synchronization on Loop Jumps**: Executing an atomic "Clean Slate" reset on transport jumps is critical in dynamic audio environments. Snapping ramps, resetting pending flags, and draining FIFOs prevents stale metadata from corrupting playback at the new transport position.

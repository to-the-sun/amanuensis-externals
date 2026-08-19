# Detailed Technical Report: Palette Buffer Read Position Calculation and Audio Playback Mechanics in `weaver~`

## Executive Summary

The `weaver~` object in Max MSP is a real-time audio synthesis and composition streaming engine. It reads control and timestamp information from a signal ramp, detects bar boundaries, queries a Max dictionary transcript (`transcript.json`), and streams slice audio from source palette buffers (`polybuffer~` / `buffer~`) into destination audio channels.

This report provides a complete, mathematically rigorous breakdown of every equation, data structure, and thread boundary transition involved in calculating the exact sample position in a palette buffer that `weaver~` reads from. Furthermore, it presents a comprehensive analysis of potential failure modes and architectural edge cases that can cause audio to be played incorrectly (or missed entirely) even when the transcript dictionary contains correct and valid values.

---

## 1. System Architecture & Thread Lifecycle

The `weaver~` system operates across two main execution domains:
1. **The High-Priority Audio/DSP Thread (`weaver_perform64` / `weaver_process_vector`)**: Runs in real time per vector, evaluates the signal ramp, updates crossfade envelopes, interpolates sample lookups, and writes into destination buffer memory.
2. **The Low-Priority Main/GUI Thread (`weaver_audio_qtask` via `qelem`)**: Handles thread-safe dictionary lookups (`dictobj`), buffer binding checks (`buffer_ref`), key parsing, and state handover back to the DSP thread.

```
+-----------------------------------------------------------------------------------+
|                                 DSP THREAD                                        |
|  1. Ramp Input -> Track Scan Position (ms)                                        |
|  2. Bar Hit Detection (Integer Division & Flooring)                               |
|  3. Enqueue Hit Event into Lock-Free Circular FIFO Queue                           |
+-----------------------------------------------------------------------------------+
                                          |
                                          v (qelem_set / audio_qtask)
+-----------------------------------------------------------------------------------+
|                                MAIN THREAD                                        |
|  4. Dequeue Hit Event from FIFO                                                   |
|  5. Format Bar Key String ("0", "2000", etc.)                                     |
|  6. Query Transcript Dictionary (dictobj_findregistered_retain)                   |
|  7. Extract "palette", "offset", "rating"                                         |
|  8. Validate Buffer Binding & Update Pending Track Metadata                       |
+-----------------------------------------------------------------------------------+
                                          |
                                          v (Handover under x->lock)
+-----------------------------------------------------------------------------------+
|                                 DSP THREAD                                        |
|  9. Swap Slot Active State (0 <-> 1)                                              |
| 10. Calculate Slot Read Offset: O_calc = O_dict - V_trig                         |
| 11. Compute Sample Index: src_ms = O_calc + v_at_f                                |
| 12. Linear Fractional Sample Interpolation                                        |
| 13. Dynamic Gain & Crossfade Ramp Processing                                      |
| 14. Write Interleaved Output to Destination Buffer                                |
+-----------------------------------------------------------------------------------+
```

---

## 2. Mathematical Equations & Step-by-Step Calculation Guide

### Step 1: Signal Ramp Normalization & Track Time
At sample frame $i$ within a DSP vector of size $N$, `weaver~` reads an input millisecond signal ramp $R[i]$ and calculates the global song scan time $V_{\text{scan}}[i]$ in milliseconds:

$$V_{\text{scan}}[i] = R[i] + M_{\text{song}}$$

Where:
- $R[i]$ is the incoming audio signal ramp value at sample $i$ (in milliseconds).
- $M_{\text{song}}$ is `x->most_negative_bar`, representing the start timestamp of the earliest bar in the song transcript (e.g. $M_{\text{song}} = -2000.0\text{ ms}$ for a song with a 1-bar pickup, or $0.0\text{ ms}$).

### Step 2: Continuous Bar Hit Detection & Floored Bar Boundary ($T_{\text{bar}}$)
Let $\text{BAR\_MS}$ be the bar length in milliseconds (obtained from the first sample of the `"bar"` buffer, e.g. $2000.0\text{ ms}$ at 120 BPM).

In `weaver_process_vector`, the current track scan time $V_{\text{scan}}$ is converted to truncated millisecond integer values:
$$r_{\text{scan}} = \lfloor V_{\text{scan}} \rfloor$$
$$r_{\text{last}} = \lfloor V_{\text{last}} \rfloor$$

When $r_{\text{scan}} \neq r_{\text{last}}$, `weaver~` checks if a bar boundary was crossed between $r_{\text{last}} + 1$ and $r_{\text{scan}}$. The floored bar boundary $T_{\text{bar}}$ (stored as `latest_j`) is computed using floor division:

$$\text{For } \text{end} \ge 0: \quad T_{\text{bar}} = \left( \lfloor \frac{\text{end}}{\text{BAR\_MS}} \rfloor \right) \times \text{BAR\_MS}$$

$$\text{For } \text{end} < 0: \quad T_{\text{bar}} = \begin{cases} \left( \frac{\text{end}}{\text{BAR\_MS}} \right) \times \text{BAR\_MS} & \text{if } \text{end} \pmod{\text{BAR\_MS}} = 0 \\ \left( \lfloor \frac{\text{end}}{\text{BAR\_MS}} \rfloor - 1 \right) \times \text{BAR\_MS} & \text{if } \text{end} \pmod{\text{BAR\_MS}} \neq 0 \end{cases}$$

Where $\text{end} = r_{\text{scan}}$.

When a valid bar boundary is detected, `weaver~` enqueues a FIFO hit entry with:
- `hit_entry.rel_time` $= T_{\text{bar}}$ (the floored bar start time, e.g. $0.0, 2000.0, 4000.0$).
- `hit_entry.bar.value` $= V_{\text{trig}}$ (the actual signal ramp scan value when the hit was detected).

### Step 3: Dictionary Lookup & Metadata Retrieval
On the main thread, `weaver_audio_qtask` formats `hit_entry.rel_time` into a string key:
$$K_{\text{bar}} = \text{snprintf}\left( \text{round}(T_{\text{bar}}) \right) \quad \longrightarrow \quad \text{e.g. } \text{"0"}, \text{"2000"}, \text{"4000"}$$

`weaver~` queries the nested dictionary:
$$\text{Dictionary}[K_{\text{track}}][K_{\text{bar}}]$$

From this dictionary object, `weaver~` extracts three primary parameters:
1. Palette symbol $P_{\text{dict}}$ (key `"palette"`, e.g. `"1.wav"` or `"palette_1.wav"`).
2. Palette offset $O_{\text{dict}}$ (key `"offset"`, e.g. $10000.0\text{ ms}$).
3. Rating scalar $R_{\text{dict}}$ (key `"rating"`, e.g. $1.0$ or $0.8$).

These are stored in the track's pending state handover structure under `x->lock`:
- `tr->pending_palette` $= P_{\text{dict}}$
- `tr->pending_offset` $= O_{\text{dict}}$
- `tr->pending_rating` $= R_{\text{dict}}$
- `tr->viz_ms` $= V_{\text{trig}}$ (the trigger ramp position)

### Step 4: DSP Slot Toggle & Calculated Read Offset ($O_{\text{calc}}$)
On the next DSP vector execution, `weaver_process_vector` inspects `tr->has_pending_data`. If active, it toggles the crossfade slot:

$$\text{active} = \text{round}(\text{tr->control}) \in \{0, 1\}$$
$$\text{other} = 1 - \text{active}$$

The crucial calculated offset $O_{\text{calc}}$ for slot `other` is computed as:

$$O_{\text{calc}}[\text{other}] = O_{\text{dict}} - V_{\text{trig}}$$

Where:
- $O_{\text{dict}}$ is the raw palette offset stored in `transcript.json` (in milliseconds).
- $V_{\text{trig}}$ is the ramp scan position captured at bar trigger time (`tr->viz_ms`).

The slot selection control parameter updates to $\text{other}$, and the crossfade direction updates:
$$\text{tr->control} = \text{other}$$
$$\text{tr->xf.direction} = \text{tr->control} - \text{tr->xf.last\_control} \in \{-1.0, +1.0\}$$

### Step 5: Real-Time Source Sample Read Position ($\text{src\_ms}$)
At destination frame $f$, corresponding to absolute ramp time $v_{\text{at\_f}}$ (in ms):

$$v_{\text{at\_f}} = f \times \frac{1000.0}{\text{sr}_{\text{dest}}}$$

The exact read timestamp $\text{src\_ms}_j$ in palette slot $j \in \{0, 1\}$ is given by:

$$\text{src\_ms}_j = O_{\text{calc}}[j] + v_{\text{at\_f}}$$

Substituting $O_{\text{calc}}[j] = O_{\text{dict}} - V_{\text{trig}}$ yields the **fundamental read equation**:

$$\bbox[10px,border:2px solid #2e6da4]{\text{src\_ms}_j = O_{\text{dict}} + (v_{\text{at\_f}} - V_{\text{trig}})}$$

This equation reveals the core design of `weaver~`: **The audio position inside the palette buffer is anchored to $O_{\text{dict}}$ at the moment $v_{\text{at\_f}} = V_{\text{trig}}$, and advances linearly $1:1$ with song playback time!**

### Step 6: Fractional Sample Index & Linear Interpolation
To convert $\text{src\_ms}_j$ into a sample frame index $f_{\text{src\_raw}}$ within palette buffer $j$:

$$f_{\text{src\_raw}} = \text{src\_ms}_j \times \frac{\text{sr}_{\text{src}}[j]}{1000.0}$$

Where $\text{sr}_{\text{src}}[j]$ is the sample rate of source buffer $j$.

The lower index $f_{\text{low}}$, upper index $f_{\text{high}}$, and fractional remainder $\text{frac}$ are computed as:
$$f_{\text{low}} = \lfloor f_{\text{src\_raw}} \rfloor$$
$$f_{\text{high}} = f_{\text{low}} + 1$$
$$\text{frac} = f_{\text{src\_raw}} - f_{\text{low}}$$

If $f_{\text{low}} \ge 0$ and $f_{\text{high}} < N_{\text{src\_frames}}[j]$, for channel $c$, samples $S_{\text{low}} = S_j[f_{\text{low}}, c]$ and $S_{\text{high}} = S_j[f_{\text{high}}, c]$ are interpolated:

$$S_{\text{interp}}[j, c] = S_{\text{low}} + \text{frac} \times (S_{\text{high}} - S_{\text{low}})$$

### Step 7: Dynamic Gain Scaling & Crossfade Ramp Synthesis
If `@dynamic_gain` is enabled ($1$), `weaver~` tracks bar ratings in a rolling time window over `song_length`.
For rating $R$:
$$\text{If } R < 0.0 \text{ and } R_{\text{min}} < 0.0: \quad G_j = 1.0 - \left( \frac{R}{R_{\text{min}}} \right)$$
$$\text{Otherwise}: \quad G_j = 1.0$$

The signal amplitude envelope $A_j$ tracks the absolute peak of $S_{\text{interp}}[j, c]$ via `ramp_process`:
- Envelope slide upwards is instantaneous (`up = 0`).
- Envelope slide downwards decays over $L_{\text{low}} = \text{low\_ms} \times \frac{\text{sr}_{\text{dest}}}{1000.0}$ samples.
- The effective crossfade length target length adaptively scales between $\text{low\_ms}$ and $\text{high\_ms}$:
  $$\text{target\_length} = \text{clip}(A_j \times \text{high\_samples}, \text{low\_samples}, \text{x->length})$$
- The normalized age of the crossfade is $\text{age} = f - \text{go}$.
- Linear fade factor $f_j = \left| \text{toggle}_j - \text{clip}\left(\frac{\text{age}}{\text{target\_length}}, 0.0, 1.0\right) \right|$.

### Step 8: Destination Frame Mapping & Output Synthesis
The output audio sample for destination channel $c$ at frame $f$ is synthesized by mixing the two slot outputs:

$$\text{Out}[f_{\text{wrapped}}, c] = \left( S_{\text{interp}}[0, c] \times f_1 \times G_0 \right) + \left( S_{\text{interp}}[1, c] \times f_2 \times G_1 \right)$$

Where $f_{\text{wrapped}}$ maps absolute song frame $f$ into the destination buffer circular space:
$$f_{\text{dest}} = f - \text{round}\left( M_{\text{song}} \times \frac{\text{sr}_{\text{dest}}}{1000.0} \right)$$
$$f_{\text{wrapped}} = f_{\text{dest}} \pmod{N_{\text{dest\_frames}}}$$

---

## 3. Comprehensive Analysis: Potential Root Causes for Audio Playback Defects

Even when `transcript.json` contains valid dictionary entries, audio playback can fail or sound incorrect. Below is an exhaustive breakdown of the architectural, mathematical, and thread-synchronization edge cases that cause playback defects.

### Defect 1: Trigger Ramp Jitter vs. Theoretical Bar Boundaries ($V_{\text{trig}}$ vs $T_{\text{bar}}$)
* **The Mechanism:** In Step 4, the offset equation is $O_{\text{calc}} = O_{\text{dict}} - V_{\text{trig}}$, where $V_{\text{trig}}$ is the ramp scan position when hit detection occurred (`hit.value`). However, Rule 1 of the specification defines theoretical audio read position as:
  $$\text{Read Position} = T_{\text{bar}} + O_{\text{dict}}$$
* **The Root Cause:** If $V_{\text{trig}}$ differs from $T_{\text{bar}}$ (for example, if hit detection occurs at $V_{\text{trig}} = 2012.3\text{ ms}$ due to vector stepping instead of exact bar boundary $T_{\text{bar}} = 2000.0\text{ ms}$), then:
  $$\text{src\_ms} = O_{\text{dict}} - V_{\text{trig}} + v_{\text{at\_f}} = O_{\text{dict}} + (v_{\text{at\_f}} - 2012.3)$$
  When $v_{\text{at\_f}}$ reaches $2012.3\text{ ms}$, $\text{src\_ms} = O_{\text{dict}}$.
  This means the audio slice starts playing **12.3 milliseconds late** relative to the palette buffer layout, causing subtle rhythmic flamming or transient smearing!
* **Corrective Equation:** If palette files are authored assuming $O_{\text{dict}}$ is anchored strictly to theoretical bar timestamp $T_{\text{bar}}$, the offset equation should use $T_{\text{bar}}$ (`hit_entry.rel_time`) rather than $V_{\text{trig}}$ (`hit.value`):
  $$O_{\text{calc}} = O_{\text{dict}} - T_{\text{bar}}$$

---

### Defect 2: Asynchronous Thread Handover Latency & Audio Skipping
* **The Mechanism:** Hit detection happens on the DSP thread. The hit is pushed to FIFO, and `qelem_set` schedules `weaver_audio_qtask` on the main thread.
* **The Root Cause:** The Max main thread is shared with GUI rendering, file I/O, and patcher logic. If the main thread experiences a delay of 20ms–50ms:
  1. The DSP thread continues processing audio vectors while waiting for dictionary metadata (`tr->waiting_for_dict = 1`).
  2. The *old* palette continues playing during this latency window.
  3. When `audio_qtask` finally runs and sets `tr->has_pending_data = 1`, the DSP thread applies $O_{\text{calc}} = O_{\text{dict}} - V_{\text{trig}}$ on vector frame $v_{\text{at\_f}} = V_{\text{now}}$ (e.g. $2040.0\text{ ms}$).
  4. The read position immediately evaluates to $\text{src\_ms} = O_{\text{dict}} + (2040.0 - 2000.0) = O_{\text{dict}} + 40.0\text{ ms}$.
* **Consequence:** The first 40ms of the new bar's audio (containing the crucial drum attack transient) is **completely skipped**, producing a truncated or quiet click instead of a punchy transient!

---

### Defect 3: Dictionary Key Format Mismatch (Symbol vs Float Keying)
* **The Mechanism:** `weaver_audio_qtask` formats the lookup key via:
  `snprintf(bstr, 64, "%ld", (long)round(hit_entry.rel_time))` $\longrightarrow$ e.g., `"2000"`.
* **The Root Cause:** In Max dictionary objects (`t_dictionary`), keys can be parsed as symbols or numeric types:
  - If a transcript generator or patcher builds the dictionary using float keys (e.g. `"2000.0"` or float numbers instead of string `"2000"`), `dictionary_getdictionary` returns `MAX_ERR_NOT_FOUND`.
  - When lookup fails, `weaver_audio_qtask` executes the fallback branch:
    `weaver_update_track_metadata(x, target_track, gensym("-"), hit.value, 0.0, bar_key, hit_entry.rel_time, 1.0);`
* **Consequence:** `weaver~` selects palette `"-"` (silence), completely muting the track for that bar despite valid data existing under key `"2000.0"`.

---

### Defect 4: Palette Filename Prefix Discrepancies
* **The Mechanism:** As documented in `TEST_GENERATION_GUIDE.md`:
  - Disk files are named with `palette_` prefix: `palette_1.wav`, `palette_2.wav`.
  - Transcript entries store stripped keys: `"palette": "1.wav"`.
* **The Root Cause:**
  - If `weaver~` searches for buffer `"1.wav"` directly using `buffer_ref_new`, but the polybuffer loaded buffers named `"palette_1"` or `"stems.1"`, buffer lookup fails (`buffer_ref_getobject` returns `NULL`).
  - `weaver~` then attempts fallback to `"stems.X"` with `fallback_offset = hit.value - x->most_negative_bar`.
* **Consequence:** The custom offset and slice from `transcript.json` are ignored, and `weaver~` falls back to playing raw linear stems audio, resulting in incorrect audio slices being played.

---

### Defect 5: Negative Integer Division Truncation in Pickup Bars
* **The Mechanism:** For pickup bars ($T_{\text{bar}} < 0$, e.g., $-2000\text{ ms}$):
  In standard C (C99 onwards), integer division `/` truncates toward zero:
  $$\frac{-1500}{2000} = 0 \quad (\text{instead of } -1)$$
* **The Root Cause:** In `weaver_process_vector`:
  ```c
  if (end < 0) {
      if (end % (long long)bar_len == 0) {
          latest_j = (end / (long long)bar_len) * (long long)bar_len;
      } else {
          latest_j = ((end / (long long)bar_len) - 1) * (long long)bar_len;
      }
  }
  ```
  If `end = -500` and `bar_len = 2000`:
  `end / bar_len` $= 0$. `(0 - 1) * 2000` $= -2000$. Correct ($T_{\text{bar}} = -2000$).
  However, if `end = -2000`:
  `end % bar_len == 0` $\to$ `latest_j = (-2000 / 2000) * 2000 = -2000`. Correct.
  **Edge Case:** If $V_{\text{scan}}$ floats between $-0.9\text{ ms}$ and $0.0\text{ ms}$, `end = (long)floor(-0.9) = -1`. `latest_j` calculates as $-2000$, enqueuing bar key `"-2000"`. One frame later when $V_{\text{scan}} = 0.1\text{ ms}$, `end = 0`, `latest_j = 0`, enqueuing bar key `"0"`. If signal noise triggers double hits near zero, bar triggers fire out of sequence!

---

### Defect 6: Sample Rate Mismatch Between Source Palette and System DSP
* **The Mechanism:** Frame lookup uses source buffer sample rate:
  $$f_{\text{src\_raw}} = \text{src\_ms}_j \times \frac{\text{sr}_{\text{src}}[j]}{1000.0}$$
* **The Root Cause:**
  - If `sr_src[j]` returns $0$ or uninitialized value before the buffer is fully loaded in Max, `sr_src` defaults to `sys_getsr()` ($44100.0\text{ Hz}$).
  - If the underlying WAV file was recorded at $48000\text{ Hz}$, the calculated sample frame index will be off by a factor of $\frac{44100}{48000} = 0.91875$.
* **Consequence:** Audio plays back at incorrect pitch and tempo (pitched down by ~1.5 semitones) and reads from the wrong physical buffer locations.

---

### Defect 7: Unbounded Crossfade Queueing & Skipped Bar Triggers
* **The Mechanism:** `weaver_process_vector` checks:
  ```c
  if ((!tr->busy || main_looped) && !tr->waiting_for_dict && r_scan != r_last)
  ```
* **The Root Cause:**
  - `tr->busy` remains `1` until crossfade ramps $r_1$ and $r_2$ complete (`r1_done && r2_done`).
  - If `@high` attribute is set to a large value (e.g. $4999.0\text{ ms}$) or if dynamic gain scaling holds ramp amplitude high, `tr->busy` stays `1` across bar boundaries.
* **Consequence:** The condition `(!tr->busy || main_looped)` evaluates to **false** at the next bar boundary ($2000\text{ ms}$ later). The object **skips the bar hit entirely**, keeping the old palette active and missing the new bar dictionary change!

---

## 4. Summary Matrix of Parameters & Calculations

| Parameter | C Symbol / Field | Source / Equation | Impact on Read Position |
| :--- | :--- | :--- | :--- |
| **Song Scan Time** | `current_scan` | $R[i] + M_{\text{song}}$ | Global timeline position (ms) |
| **Bar Start Time** | `hit_entry.rel_time` | $\lfloor \text{end} / \text{BAR\_MS} \rfloor \times \text{BAR\_MS}$ | Formats dictionary lookup key $K_{\text{bar}}$ |
| **Trigger Time** | `hit.value` / `tr->viz_ms` | $V_{\text{trig}}$ (captured scan time) | Offset anchor subtraction point |
| **Dict Offset** | `tr->pending_offset` | `"offset"` key in `transcript.json` | Theoretical palette start position |
| **Calculated Offset**| `tr->offset[j]` | $O_{\text{dict}} - V_{\text{trig}}$ | Static offset added to vector scan time |
| **Read Position** | `src_ms` | $O_{\text{dict}} + (v_{\text{at\_f}} - V_{\text{trig}})$ | Physical millisecond location in WAV |
| **Read Frame** | $f_{\text{src\_raw}}$ | $\text{src\_ms} \times \frac{\text{sr}_{\text{src}}}{1000.0}$ | Exact sample index for linear interpolation |
| **Interpolated Sample**| $S_{\text{interp}}$ | $S[f_{\text{low}}] + \text{frac}(S[f_{\text{high}}] - S[f_{\text{low}}])$ | Sub-sample accurate audio output |

---

## 5. Architectural Recommendations

1. **Synchronize Offset to Theoretical Bar Boundaries ($T_{\text{bar}}$):**
   Modify `weaver_process_vector` to calculate $O_{\text{calc}}$ relative to `hit_entry.rel_time` ($T_{\text{bar}}$) rather than `hit.value` ($V_{\text{trig}}$) to eliminate sub-vector jitter:
   $$O_{\text{calc}} = O_{\text{dict}} - T_{\text{bar}}$$
2. **Robust Key Parsing:**
   In `weaver_audio_qtask`, fallback to alternative string formats (e.g. checking both `"2000"` and `"2000.0"`) if `dictionary_getdictionary` fails.
3. **Explicit Busy Timeout Guard:**
   Ensure `tr->busy` is force-cleared or capped if crossfade length exceeds bar duration to prevent missing subsequent bar triggers.

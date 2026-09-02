# Architectural Report: Center-Anchored Stems Buffers and Negative Time Simplification

## 1. Executive Summary and Paradigm Shift

In musical timeline management systems that support bidirectional growth (expanding into negative bar timestamps before the original song start as well as positive timestamps after), a persistent challenge has been handling non-zero or negative origins within zero-indexed audio buffers. Max audio buffers (`buffer~`, `polybuffer~`) and standard sampling structures are non-negative indexed arrays starting at frame 0 (0 ms).

Historically, the system accommodated negative bars by calculating a dynamic global metric known as `most_negative_bar` (the most negative timestamp present across all tracks, e.g., -16000 ms). Whenever `most_negative_bar` expanded further in the negative direction, external objects (`crucible`, `weaver~`, `smartloop~`) and transcript dictionary entries had to apply floating-point offsets (such as `offset = -most_negative_bar`) to real-time audio ramps, destination write frames, and stems buffer lookups.

### The Proposed Paradigm: Center-Anchored Stems Buffers
Instead of dynamically shifting buffer zero-points and adjusting dictionary offsets whenever a new negative bar is introduced, stems buffers and destination audio buffers are pre-allocated with liberal storage capacity (for example, a 20-minute audio buffer) where the song's origin (timestamp 0 ms) is anchored directly at the buffer's center point (e.g., frame index corresponding to 10 minutes or 600,000 ms).

Under this architecture:
- Timestamp `0 ms` corresponds to a fixed center offset $T_{\text{center}}$ in all stems and destination buffers.
- Positive bar timestamps (e.g., `+4000 ms`) map directly to $T_{\text{center}} + 4000\text{ ms}$.
- Negative bar timestamps (e.g., `-8000 ms`) map directly to $T_{\text{center}} - 8000\text{ ms}$.

Because the stems buffers have ample headroom in both directions from $T_{\text{center}}$, the song can expand flexibly into negative or positive time without shifting existing buffer contents or recalculating stems offsets across transcript dictionaries.

---

## 2. Impact on the Crucible Object (`crucible`)

### Current State
When `@rescore` is enabled in `crucible`, incoming `absolutes` and `scores` selector messages for missing bars trigger the creation of a new bar entry in `incumbent_dict`. Under the legacy approach, `crucible` calculates `most_negative_bar` across all active bars in the song and sets the bar's `offset` key to `-most_negative_bar`. If a newer bar expands `most_negative_bar` even further, existing stems offsets become invalidated or require re-basing.

### Center-Anchored Simplification
1. **Constant Offset of Zero for Stems Bars:**
   When `@rescore` creates a new stem bar (with `palette = stems.[track_id]`), its `offset` key is simply set to `0.0` (or $T_{\text{center}}$ if absolute stem playback alignment is used) rather than `-most_negative_bar`.
2. **Elimination of Global `song_min` Re-basing:**
   `crucible` no longer needs to scan the entire incumbent dictionary to find `song_min` solely to calculate a stems offset.
3. **Transcript Dictionary Invariance:**
   Bar dictionary entries remain completely static during song growth. Adding a bar at -32000 ms does not alter the stored offset of existing bars at -8000 ms or +16000 ms.
4. **Simplified Reach Calculations:**
   While `crucible` continues to compute `song_reach` (total span from lowest bar to highest bar plus bar length) for UI status and progress outlets, reach calculations no longer mutate data dictionary attributes.

---

## 3. Impact on the Weaver Object (`weaver~`)

The `weaver~` object is responsible for sample-accurate weaving of audio from external palette buffers into a named destination `polybuffer~` in real-time based on an incoming signal time ramp.

### Current State
In `weaver~.c`:
- **Ramp Processing:** The incoming time ramp sample value `ramp_in[i]` is continuously offset by adding `x->most_negative_bar` (`current_scan = ramp_in[i] + x->most_negative_bar`) to convert negative ramp positions into positive buffer sample indices.
- **Destination Buffer Write Frame:** The destination write sample index `f_dest` requires subtracting `f_offset` (derived from `x->most_negative_bar` scaled to destination sample rate).
- **Stems Fallback Offset:** When a bar is missing or its palette buffer cannot be found, `weaver~` falls back to `stems.[track_id]`. It computes `song_ms_offset = hit.value - x->most_negative_bar` and sets `fallback_offset = -x->most_negative_bar`.
- **Dynamic Attachment Checks:** `weaver_update_most_negative_bar()` continuously inspects the transcript dictionary to update `x->most_negative_bar`.

### Center-Anchored Simplification
1. **Direct Ramp Mapping:**
   The incoming time ramp `ramp_in[i]` can be processed directly without adding a dynamic `most_negative_bar` scalar.
2. **Fixed Destination Write Frame Indexing:**
   Destination frame calculations become $f_{\text{dest}} = \text{round}((current\_scan + T_{\text{center}}) \times \text{sample\_rate} / 1000.0)$. The offset $T_{\text{center}}$ is constant and independent of the active transcript state.
3. **Streamlined Fallback to Stems:**
   When falling back to `stems.[track_id]`, the read position in the stems buffer aligns 1:1 with the destination write position. The offset for stems buffers is uniformly `0.0` relative to the center anchor $T_{\text{center}}$.
4. **Consolidation Worker Thread:**
   In `weaver_consolidate_worker()`, the simulated ramp processing loop no longer needs to query or adjust track-specific `most_negative_bar` limits. The consolidation timeline operates on plain song time centered at $T_{\text{center}}$.

---

## 4. Impact on the Smartloop Object (`smartloop~`)

The `smartloop~` object monitors audio time ramps to detect jumps, loops, and timeline discontinuities, and periodically scans transcript dictionaries to identify optimal loop regions.

### Current State
In `smartloop~.c`:
- `smartloop_perform64()` converts incoming DSP ramp samples via `val = in[i] + x->most_negative_bar`.
- Dynamic changes in `x->most_negative_bar` require explicit delta adjustments (`x->last_val += (x->most_negative_bar - x->last_most_negative_bar)`) to prevent false positive jump detections when the song grows in the negative direction.
- Start and end loop outputs (`out_start`, `out_end`) map internal boundaries back by subtracting `x->most_negative_bar`.

### Center-Anchored Simplification
1. **Direct DSP Signal Inspection:**
   `smartloop~` inspects incoming ramp values directly (`val = in[i]`).
2. **Elimination of False Jump Compensation:**
   Because song expansion in the negative direction no longer shifts the origin or updates `most_negative_bar`, DSP state routines do not need special-case logic to adjust `x->last_val`.
3. **Simplified Outlet Reporting:**
   Loop start and end millisecond timestamps output directly as song-relative time (e.g., `-8000.0` to `+16000.0`) or as direct offsets relative to $T_{\text{center}}$, removing runtime subtraction.

---

## 5. Buffer Writing and Recording Logic (Max Patchers & System Level)

### Current State
When recording live audio or stem tracks into Max session buffers (`stems.[track_id]`), recording write heads had to be dynamically re-indexed or audio samples shifted whenever new negative bars were inserted prior to time 0.

### Center-Anchored Simplification
1. **Fixed Recording Origin:**
   The recording process begins at $T_{\text{center}}$ (representing song timestamp 0 ms).
2. **Symmetrical Growth:**
   If the user records a pickup measure or count-in prior to the song start, the write head simply writes to $T_{\text{center}} - t_{\text{pickup}}$.
3. **No Sample Re-shifting:**
   Audio recorded at timestamp +4000 ms remains at frame $T_{\text{center}} + 4000\text{ ms}$ indefinitely, regardless of how many negative bars are subsequently added.

---

## 6. Impact on Rebar, Buildspans, and Visualizers (`visualizer.py`)

### Rebar and Buildspans
- `crucible_do_rebar` calculates bar quantization via `val = abs_val - offset`. With stem offsets fixed to `0.0` (or $T_{\text{center}}$), timestamp quantization remains stable across rebar operations.
- `buildspans` handles note absolute timestamps (`looped_absolute`). Offset invariance simplifies span validation and discontiguity checks across lingering spans.

### Visualizer (`visualizer.py`)
- The Python visualizer currently scans all active tracks to calculate `most_negative_bar` and `most_positive_bar` to derive layout pixel coordinates (`total_time_span = most_positive_bar_plus_len - most_negative_bar`).
- Under a center-anchored system, `most_negative_bar` remains a pure layout visual start boundary for drawing grid cells on screen, completely decoupled from buffer write indexing or C-core audio rendering offsets.

---

## 7. Buffer Sizing Guidelines and Practical Considerations

To ensure the center-anchored approach functions reliably without running out of buffer space:

1. **Pre-Allocation Guidelines:**
   - Standard stems buffers should be pre-allocated with generous capacity (e.g., 20 minutes / 1,200,000 ms at 44.1 kHz or 48 kHz).
   - $T_{\text{center}}$ is designated at half the buffer duration (e.g., 10 minutes / 600,000 ms).
2. **Headroom Capacity:**
   - A 20-minute buffer centered at 10 minutes allows up to 10 minutes of negative timeline expansion and 10 minutes of positive timeline expansion before buffer resizing or boundary wrapping would ever be required.
3. **Memory Footprint:**
   - A 20-minute 32-bit float mono audio buffer consumes approximately 105 MB of RAM. Modern digital audio workstations and Max environments easily handle this memory footprint across multiple stems tracks.

---

## 8. Comparative Analysis Matrix

| System Component | Legacy Dynamic Shift Paradigm | Center-Anchored Stems Paradigm |
| :--- | :--- | :--- |
| **Stems Buffer Origin** | Frame 0 represents `most_negative_bar` | Frame $T_{\text{center}}$ represents 0 ms |
| **`crucible` `@rescore` Offset** | Sets `offset = -most_negative_bar` | Sets `offset = 0.0` |
| **Transcript Dict Stability** | Dict offsets mutated on negative song growth | Dict offsets remain completely invariant |
| **`weaver~` Ramp Processing** | `ramp_in + most_negative_bar` | Direct ramp processing (`ramp_in + T_center`) |
| **`weaver~` Stems Fallback** | Dynamic offset calculation (`-most_negative_bar`) | Fixed 1:1 mapping with center offset |
| **`smartloop~` Jump Detection** | Requires compensation for shifting `most_negative_bar` | Zero false-jump risk on negative expansion |
| **Buffer Sample Shifting** | Required when song grows negative | Never required |

---

## 9. Conclusion

Transitioning to a center-anchored stems buffer architecture eliminates complex offset cascades across C external objects (`crucible`, `weaver~`, `smartloop~`), prevents transcript dictionary mutations during song growth, and simplifies real-time DSP ramp processing. By establishing a fixed origin $T_{\text{center}}$ within pre-allocated stems buffers, the system gains complete flexibility to expand in both positive and negative time directions without runtime overhead or sample-shifting artifacts.

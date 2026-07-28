# Detailed Analysis: Weaving Visualization and Negative Territory Graphing Issues

This report provides a comprehensive, technically detailed analysis of how the weaving process is rendered in the lower graph of `debug_visualizer.py` and diagnoses why bars below zero fail to graph properly. Specifically, we investigate why negative bars are often scrunched together and not taken from where they should be playing.

---

## 1. Weaver Visualization in `debug_visualizer.py`

In `debug_visualizer.py`, the weaving process is displayed in the lower graph via the `draw_weaver` function. This graph visualizes real-time and offline audio consolidation behavior across multiple parallel tracks.

### 1.1 Data Structures & Rendering Pipeline
The GUI is built using Pygame and operates on a shared state dictionary. The state of the weaver is defined by:
- `state["data_points_by_track"]`: Envelopes or continuous values ($f_1, f_2$) representing crossfade gains and audio signals on each track.
- `state["labels_by_track"]`: Lists of segment boundaries (labels) denoting when a track switches to a different palette and offset (e.g., `palette@offset`).
- `state["track_lengths"]`: Absolute lengths of each track in milliseconds.
- `state["busy_states"]`: Booleans indicating if a track is active/transitioning.
- `state["global_min_ms"]` and `state["global_max_ms"]`: Boundaries representing the scanned timeline range.

### 1.2 Coordinate Mapping & Graph Layout
The `draw_weaver` function divides the lower half of the screen (`600` pixels high, scaled by a factor of `0.8`) into multiple rows, one for each track seen in the timeline:
1. **Vertical Bounds:**
   - Pad heights (`top_pad`, `bottom_pad`, and `row_spacing`) are applied.
   - For $N$ tracks, each track is allocated a row of height:
     $$\text{row\_full\_h} = \frac{\text{graph\_h}}{N}$$
   - The active drawing row graph height is:
     $$\text{row\_graph\_h} = \text{row\_full\_h} - \text{row\_spacing}$$
2. **Horizontal Bounds:**
   - Active horizontal drawing region goes from `left_pad` to `w - right_pad`.
   - The timeline duration (span) being viewed is:
     $$\text{view\_span\_ms} = \text{view\_end\_ms} - \text{view\_start\_ms}$$
   - For any millisecond timestamp $t$, its horizontal screen coordinate $x$ is mapped linearly:
     $$x = \text{left\_pad} + \frac{t - \text{view\_start\_ms}}{\text{view\_span\_ms}} \times \text{graph\_w}$$

### 1.3 Lower Graph Rendering Steps
For each track, the following components are drawn sequentially:
- **Shaded Segment Backgrounds:** The function iterates through sorted track labels. Each label denotes the start of an interval playing `palette@offset`. The width of this interval is drawn as a colored alpha-blended rectangle spanning from the label's `ms` coordinate to the next label's `ms` (or `view_end_ms`). The color is deterministically hashed from the `palette@offset` string using `get_color_for_label`.
- **Ramp/Crossfade Curves ($f_1, f_2$):** Continuous lines are drawn for $f_1$ (green, representing the active slot's gain) and $f_2$ (red, representing the incoming slot's gain) by connecting successive points in `data_points_by_track`.
- **Label Indicators & Text:** Vertical lines (colored `(100, 100, 120)`) are drawn at each label timestamp. Rhythmic bar indices and palette/offset strings are rendered as tiny text labels in alternating top/bottom positions (`pos_idx = 0` or `1`) within the track row to avoid overlaps.
- **Track Metadata Sidebar:** A label on the left margin displays the track index and its resolved length in milliseconds. If a track is in a "busy" transition state, its label background flashes white.
- **Timeline Ticks & Axis:** A horizontal scale with 10 equal divisions is rendered along the bottom edge, displaying absolute milliseconds (e.g. `-2000 ms`, `0 ms`, `5000 ms`).

---

## 2. The Behavior of Bars Below Zero

When the weaver operates on a timeline containing negative bar timestamps (which occur when the transcript dictionary contains bars representing time before the zero-point of the main ramp), the mathematical logic in the DSP thread changes. This leads to a fundamental divergence between positive and negative time regions.

### 2.1 C Integer Division vs. Mathematical Floor Division
In C, the division operator `/` between two integers **truncates toward zero**.
In contrast, mathematical floor division always **rounds down toward negative infinity** (represented by `floor` in floating-point math).

This difference is critical in `weaver~/weaver~.c` within the **Continuous Bar Hit Detection** block:

```c
// Line 1230 in weaver~.c (Continuous detection)
long long latest_j = (end / (long long)bar_len) * (long long)bar_len;
```

Compare this to the **Initial Bar Trigger** block, which correctly utilizes floating-point mathematical floor division:

```c
// Line 1246 in weaver~.c (Initial bar trigger)
double initial_bar = floor(tr_scan / bar_len) * bar_len;
```

### 2.2 Mathematical Divergence: Positive vs. Negative Territory
Let us analyze how these two equations behave with a bar length of `100 ms`.

#### Case A: Positive Territory ($t = 150\text{ ms}$)
During steady-state forward playback, `r_last = 149` and `r_scan = 150`.
- `end` = `150`.
- `start` = `r_last + 1` = `150`.
- `latest_j` (C Division) = `(150 / 100) * 100` = `100`.
- **Comparison:** Is `latest_j >= start` ($100 \geq 150$)? **False**.
- **Outcome:** No bar trigger is fired. A trigger will *only* fire when `r_scan` reaches `200`, because `latest_j` becomes `200` and `start` is `200` ($200 \geq 200$, which is **True**). At `201`, `latest_j` is still `200` but `start` becomes `201` ($200 \geq 201$, which is **False**). This is correct: **exactly one trigger is fired per bar boundary**.

#### Case B: Negative Territory ($t = -150\text{ ms}$)
During steady-state forward playback in negative territory, `r_last = -151` and `r_scan = -150`.
- `end` = `-150`.
- `start` = `r_last + 1` = `-150`.
- `latest_j` (C Division) = `(-150 / 100) * 100` = `-1 * 100` = `-100`.
- **Comparison:** Is `latest_j >= start` ($-100 \geq -150$)? **True**!
- **Outcome:** A bar trigger is fired!
- **The Redundancy Cascade:**
  On the next millisecond, `r_last = -150` and `r_scan = -149`:
  - `end` = `-149`.
  - `start` = `r_last + 1` = `-149`.
  - `latest_j` (C Division) = `(-149 / 100) * 100` = `-1 * 100` = `-100`.
  - **Comparison:** Is `latest_j >= start` ($-100 \geq -149$)? **True**!
  - **Outcome:** A redundant bar trigger is fired again!

Because `-100` is algebraically greater than any value from `-150` to `-100`, the condition `latest_j >= start` remains **True for every single millisecond step** in that entire 50 ms window!

---

## 3. Why Bars Below Zero Do Not Graph Properly

The mathematical error described above causes the lower graph visualization to completely break down in negative territory.

### 3.1 The Millisecond Trigger Flood
Because `latest_j >= start` evaluates to **True** on every single sample vector where the millisecond counter advances in negative territory, `weaver~` floods the TCP socket with a continuous stream of redundant labels and data packets. Instead of sending one label update per bar (e.g. once every 125 ms), it sends a new label update **every single millisecond**!

### 3.2 Label Queue Overflow and History Purging
In `debug_visualizer.py` (line 116), the incoming state buffer has a hard limit of `1000` labels per track to prevent infinite memory growth:

```python
# Line 116 in debug_visualizer.py
if len(state["labels_by_track"][track_id]) > 1000:
    state["labels_by_track"][track_id].pop(0)
```

- When the weaver is in negative territory, a single track can generate `1000` redundant bar labels in just **one second** of playback.
- This flood of redundant labels instantly fills the queue, triggering the `pop(0)` mechanism.
- Consequently, all older, historically correct labels (such as those representing positive times or the original structure of the track) are **permanently deleted and purged from the visualizer state**.

### 3.3 Loop Boundary Skipping
When a loop boundary occurs in negative territory, the `start` variable is set to `0`:
```c
long long start = (track_looped || main_looped) ? 0 : r_last + 1;
```
If we are in negative territory, `latest_j` is negative (e.g., `-100`).
- **Comparison:** Is `latest_j >= start` ($-100 \geq 0$)? **False**.
- **Outcome:** The loop bar trigger is completely skipped, meaning the first bar of a looped section in negative territory is never requested from the dictionary and fails to play or render.

---

## 4. The "Scrunching" Phenomenon Explained

Users observe that below zero, many bars appear **scrunched into the same area** and are not taken from where they should actually be playing from. We can now pinpoint the exact mechanical reasons for this phenomenon:

1. **Temporal Density of Redundant Labels:** Because a trigger is fired on every millisecond, the visualizer receives hundreds of distinct segments that are only `1 ms` wide.
2. **Visual Cluttering:** The `draw_weaver` function draws a shaded background interval and an alternating label for *every* item in `state["labels_by_track"]`. Since there are 1000 labels representing a tiny 1-second range, they are squeezed into a few pixels on the screen. The text labels and background rectangles overlap, producing a dense, illegible "scrunched" block of solid color.
3. **Loss of Correct Reference Offsets:** Because the 1000-element queue is exhausted almost instantly by the redundant flood, the older labels representing the actual long-term track structure are deleted. The visualizer no longer knows about the true historical bars, so it displays empty space for most of the timeline and crams the entire rendering buffer into the small active negative window.
4. **Incorrect Playback Offsets:** The endless triggering forces `weaver~` to continually overwrite track slots and re-request data from the dictionary on every single sample. Since the division is incorrect, `weaver~` requests incorrect bar dictionary keys (or falls back to silence/default streams), meaning the audio is not taken from the correct temporal positions inside the source palettes.

---

## 5. Conclusion

The visualization failures and audio scrunching below zero are caused by a chain reaction:
$$\text{C Truncated Division} \implies \text{Continuous Triggering} \implies \text{TCP Queue Flood} \implies \text{Visualizer Buffer Purge}$$

By using C's truncated integer division for negative numbers in the continuous bar check, the logic assumes positive-number behavior where `latest_j` is always less than or equal to `start` until a boundary is crossed. In negative territory, this assumption is reversed, creating a continuous feedback loop of redundant triggers that overwrites the visualization history and causes severe visual and sonic degradation.

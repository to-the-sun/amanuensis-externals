# Bug Report: Systemic `loop_start` Offset Mismatch and Premature Discontiguity in `buildspans`

## 1. Executive Summary
An in-depth code-level audit of the `buildspans` object has confirmed a critical, systemic coordinate mismatch bug related to the handling of the `loop_start` offset. This bug manifests in two distinct but deeply related ways:
1. **Premature Discontiguity (Offset Changes):** When a global offset change is processed in `buildspans_do_offset`, the discontiguity check is performed using coordinate systems with different baselines. This leads to false gap detections and premature span termination/flushing.
2. **Systemic Span Key Offset Mismatch:** Timestamps stored under the `span` key in the `building` dictionary are shifted by `loop_start`, causing downstream objects (like `crucible`, which has no awareness of `loop_start`) to store and process incorrect timeline values. The visualizer's correct representation is actually a result of an "un-shifting" compensation workaround.

---

## 2. Walkthrough of the Mismatch (The Log vs. Visualizer Discrepancy)

### Scenario Setup
* **Loop Start:** `loop_start = -3447`
* **Incoming Absolute Note Time (`calc_timestamp`):** `6894`
* **Current Offset (`offset`):** `0`

### Step 1: Note Intake and Storage in `buildspans.c`
In `buildspans_process_and_add_note()`, the relative bar timestamp is calculated as:
```c
double relative_timestamp = calc_timestamp - offset + x->loop_start;
long bar_timestamp_val = floor(relative_timestamp / bar_length) * bar_length;
```
Substituting the scenario values:
$$\text{relative\_timestamp} = 6894 - 0 + (-3447) = 3447$$
So, the bar timestamp is calculated and stored as **`3447`**. All parallel arrays (`absolutes`, `scores`, `span`) in the `building` dictionary for this bar are keyed under this relative bar timestamp of **`3447`** (and subsequently `4596`, `5745`).

### Step 2: Visualization Handling in `debug_visualizer.py`
When `buildspans` sends its memory state packet to `debug_visualizer.py` (via `buildspans_visualize_memory()`), the packet includes the raw keys (`3447, 4596, 5745`) and the global `loop_start` (`-3447`).

The visualizer's layout logic attempts to compute the absolute timeline position to display them correctly:
```python
bar_abs_start_ts = (bar_relative_ts - loop_start) + offset_val
```
Substituting the scenario values:
$$\text{bar\_abs\_start\_ts} = 3447 - (-3447) + 0 = 6894$$
The visualizer successfully displays the bar at **`6894`** (and `8043`, `9192`).

### The Problem
The visualizer was displaying the correct **absolute timeline** positions (`6894, 8043, 9192`), but the actual data stored in `buildspans` (and emitted to downstream Max patches via Outlet 1) is shifted/corrupted by `loop_start` (`3447, 4596, 5745`).

---

## 3. The Premature Discontiguity Bug

During a global offset update (e.g. playhead jump or loop iteration), `buildspans_do_offset()` triggers a discontiguity check for active tracks *before* duplicating spans:

```c
double relative_f = f - track_offset;
buildspans_check_discontiguity(x, gensym(pal_str), gensym(track_str), relative_f);
```
Where `f` is the absolute playhead offset.

In `buildspans_check_discontiguity()`:
```c
double gap_limit = (double)most_recent_bar_after_rating_check + 2.0 * (double)bar_length;
int is_discontiguous = (relative_comparison_val > gap_limit);
```

### The Coordinate System Conflict
1. `most_recent_bar_after_rating_check` is retrieved from the dictionary keys. As proved above, these keys are **shifted by `loop_start`** (e.g., **`3447`**).
2. `relative_comparison_val` (passed as `relative_f`) is purely `f - track_offset`. It is **NOT shifted by `loop_start`** (e.g., **`6894`**).

### Mathematical Breakdown of the Failure
$$\text{gap\_limit} = 3447 + 2 \times 125 = 3697$$
$$\text{relative\_comparison\_val} = 6894$$
$$\text{is\_discontiguous} = (6894 > 3697) \implies \mathbf{True}$$

Even though the note at `6894` is contiguous in absolute terms, the coordinate mismatch causes `buildspans` to detect a giant gap, prematurely ending and flushing the current span.

---

## 4. Absolute vs. Relative Timelines in standard Max Objects and `visualizer.py`

In Max MSP at large, absolute timestamps are required for playback buffer indexing because buffer objects (`buffer~`, `polybuffer~`) cannot have negative indices. Consequently, all audio buffers are positive-indexed (i.e. zero-based), which represents an absolute timeline starting at zero.

### `weaver~` and `most_negative_bar`
To resolve this, standard external objects like `weaver~` and `smartloop~` calculate a `most_negative_bar` offset mapping representing the lowest bar timestamp scanned track-wide/song-wide.
* **Timeline Mapping:** `weaver~` adds the detected `most_negative_bar` offset to the incoming time ramp on its first inlet, and subtracts its frame equivalent from the sample index values of the destination stems buffers before writing to them, allowing the outside Max patch to work with standard positive buffer indices.
* **Playback Offsets:** When fallback buffers (such as `stems.x`) are used, the playback offset is calculated as `hit.value - most_negative_bar` to properly align positive-indexed buffers with the absolute timeline.

### `visualizer.py` and Hash Marks
The standard `visualizer.py` aligns note hash marks with negative bar boundaries dynamically.
* It scans the repopulated local database to calculate the absolute minimum bar timestamp across all active tracks (the `most_negative_bar`).
* If a relative note timestamp (`rel_ms`) is less than zero, `visualizer.py` adds `most_negative_bar` to the relative note timestamp `rel_ms` to map it correctly into positive visualization space.

---

## 5. Hash Mark Positioning in `debug_visualizer.py`

In `debug_visualizer.py`, the layout and coordinate mapping of individual note hash marks (depicting absolute note arrival times and parallel scores) are drawn using a linear timeline mapping.

### Coordinate Boundaries (`min_ts`, `max_ts`)
First, the visualizer gathers all absolute note timestamps (`absolutes`) and bar offsets (`offsets`) across all active tracks and the current playhead offset (`current_offset`) to find the global minimum and maximum timeline boundaries:
```python
all_ts = [
    ts
    for track_data in working_memory.values()
    for key, ts_list in track_data.items()
    if key in ("absolutes", "offsets")
    for ts in ts_list
]
if current_offset is not None:
    all_ts.append(current_offset)
```
These values establish the overall timeline span (`span_ts = max_ts - min_ts`).

### Mapped X Coordinates
Every note stored in a bar's `absolutes` array contains its high-precision timestamp `ts`. The visualizer projects this time value to an X pixel coordinate using a linear ratio:
```python
x = grid_left + grid_w * (ts - min_ts) / span_ts
```
Where:
* `grid_left`: The visual left margin boundary of the timeline grid.
* `grid_w`: The total drawable pixel width of the timeline grid (`grid_right - grid_left`).
* `ts - min_ts`: The relative distance of the note from the start of the visible timeline.
* `span_ts`: The total time duration currently spanning the visible timeline.

### Note Rendition
At the calculated pixel coordinate `x`, the visualizer renders:
1. **Vertical Ticks:** A solid green vertical tick is drawn across the full height of the track row to align notes accurately on the timeline:
   ```python
   pygame.draw.line(surface, (100, 200, 100), (x, track_y), (x, track_y + track_h), 1)
   ```
2. **Text Labels:** A text label representing the note's precise double timestamp value (`f"{ts:.2f}"`) is rendered directly adjacent to the tick mark.
3. **Score Pop-up Boxes:** To render score data associated with the note (`S: score_val`), the visualizer retrieves the parallel `scores` array element. A custom pop-up box is constructed with a dark background and a solid green border, centered horizontally on coordinate `x` and positioned directly above the track baseline.

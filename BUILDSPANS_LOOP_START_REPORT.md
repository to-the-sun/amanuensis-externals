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
The visualizer is displaying the correct **absolute timeline** positions (`6894, 8043, 9192`), but the actual data stored in `buildspans` (and emitted to downstream Max patches via Outlet 1) is shifted/corrupted by `loop_start` (`3447, 4596, 5745`).

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

## 4. Downstream Systemic Failure with `crucible`

The `crucible` object is bound downstream to receive spans output by `buildspans` on Outlet 1.
```c
if (s == gensym("span") && argc > 0) {
    t_atomarray *span_aa = atomarray_new(argc, argv);
    crucible_process_span(x, x->last_track_id, span_aa);
}
```
* **Crucible has NO `loop_start` attribute.**
* It receives the raw relative list of span bars from `buildspans` (e.g., `[3447, 4596, 5745]`).
* Because it lacks `loop_start`, it treats `3447` as the absolute timeline coordinate.
* Any rebar operations (`rebar`), transience processing, or consolidation inside `crucible` are now executing on corrupted timeline data shifted by `loop_start`.
* When `crucible` serializes its state for `visualizer.py` (via repopulation), it sends `3447` without any `loop_start` context. The standard `visualizer.py` (which has no `loop_start` handler) displays the `crucible` bars incorrectly at `3447` instead of `6894`.

---

## 5. Propose Code-Level Fixes

### Fix 1: Align Coordinate Systems in Discontiguity Check
To resolve the immediate premature discontiguity bug, `relative_f` must incorporate `loop_start` to match the coordinate system of the stored keys.

In `buildspans_do_offset()`:
```c
<<<<<<< SEARCH
                        double track_offset = (double)atol(dash + 1);
                        double relative_f = f - track_offset;
                        buildspans_check_discontiguity(x, gensym(pal_str), gensym(track_str), relative_f);
=======
                        double track_offset = (double)atol(dash + 1);
                        double relative_f = f - track_offset + x->loop_start;
                        buildspans_check_discontiguity(x, gensym(pal_str), gensym(track_str), relative_f);
>>>>>>> REPLACE
```

### Fix 2: Systemic Alignment (Passing loop_start Downstream)
If `loop_start` is mathematically required for the relative timeline, downstream objects (specifically `crucible`) must be made aware of it to correctly interpret and serialize bar spans.

Alternatively, `buildspans` should calculate bar timestamps *without* `loop_start` internally (keeping absolute alignment), and only utilize `loop_start` as a playback/display offset.

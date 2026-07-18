# Technical Report: The `@fill` Attribute in the `crucible` Object

This report outlines the technical design, implementation, and exact step-by-step processing lifecycle of the `@fill` attribute in the `crucible` MaxMSP external object. It details how padding/filling is handled inside the C external, how it communicates with the downstream Python visualizer, and how it interacts with reach calculations.

---

## 1. Overview & Purpose of `@fill`

The `@fill` attribute in the `crucible` object is an optional feature designed to keep all tracks synchronized in length across the song's absolute timeline.

When a track's length grows in either the positive or negative direction (due to a challenger span winning and extending that track beyond the previous boundaries of the song), and `@fill` is enabled (`1`), `crucible` automatically pads or "fills" all other "lesser" tracks in that direction. This ensures that every track spans the exact same timeline from the earliest global bar to the latest global bar.

The padding is accomplished by copying the existing bars of the shorter tracks forward (for positive growth) or backward (for negative growth) to fill the gaps.

> ⚠️ **Warning Notice in Reference Documentation**: As noted in `crucible.maxref.xml`, the `@fill` attribute is marked with a warning stating that this automated filling feature is unfinished and may produce unexpected behavior under specific musical structures.

---

## 2. Technical Declaration & Attributes

### Inside `crucible.h`
The attribute is defined inside the main state structure `t_crucible`:
```c
long fill;             // Object's fill attribute state (0 or 1)
void *outlet_fill;     // Dedicated Max MSP outlet for fill notifications
```

### Inside `crucible.c`
The attribute is registered with Max's class initialization routine using:
```c
CLASS_ATTR_LONG(c, "fill", 0, t_crucible, fill);
CLASS_ATTR_STYLE_LABEL(c, "fill", 0, "onoff", "Enable Fill");
CLASS_ATTR_DEFAULT(c, "fill", 0, "0");
CLASS_ATTR_ACCESSORS(c, "fill", NULL, (method)crucible_attr_set_fill);
```

The accessor setter function logs the update:
```c
t_max_err crucible_attr_set_fill(t_crucible *x, void *attr, long ac, t_atom *av) {
    if (ac && av) {
        x->fill = atom_getlong(av);
        crucible_log(x, "fill attribute set to %ld", x->fill);
    }
    return MAX_ERR_NONE;
}
```

---

## 3. Detailed Step-by-Step Execution Flow

The `@fill` mechanism is triggered exclusively inside `crucible_process_span` when a **challenger span wins** and is successfully written to the incumbent dictionary.

Below is the exact step-by-step process of how `@fill` operates:

### Step 3.1: Copy Winning Bars to Incumbent
Before filling begins, the winning challenger span's bars are copied into the incumbent track dictionary, establishing the new track bounds.

### Step 3.2: Verify Attribute and Dictionary State
The system checks if `x->fill` is enabled and that the `incumbent_dict` exists:
```c
if (x->fill && incumbent_dict) { ... }
```

### Step 3.3: Calculate Global Song Bounds
The object iterates over all tracks currently in the `incumbent_dict` to find the absolute global start and end bounds of the entire song:
- It calls `get_track_bounds` on each track to find the track's minimum (`t_min`) and maximum (`t_max`) bar timestamps.
- It finds the global minimum (`song_curr_min`) and global maximum (`song_curr_max`) across all tracks.
- It prints a debug log: `Filling tracks to match song bounds: [song_curr_min, song_curr_max]`.

### Step 3.4: Iterate Over All Tracks to Detect Gaps
The system loops through all tracks (`all_track_keys[t]`) and fetches their specific local bounds (`o_min` and `o_max`).

---

### Step 3.5: Positive Growth Padding (Forward Copying)
If a track's local maximum `o_max` is strictly less than the global maximum `song_curr_max`:
1. **Retrieve Sorted Bars**: It retrieves the track's existing bar timestamps, sorted numerically:
   ```c
   t_atom_long *o_bars = get_sorted_track_bars(other_track_dict, &o_bars_count);
   ```
2. **Loop to Fill Gaps**: It loops `dest_ts` starting from `o_max + bar_length` up to `song_curr_max`, incrementing by `bar_length`:
   ```c
   for (t_atom_long dest_ts = o_max + bar_length; dest_ts <= song_curr_max; dest_ts += bar_length) { ... }
   ```
3. **Select Source Bar (Modulo indexing)**: It wraps around the sorted array from start to end using a forward modulo counter:
   ```c
   t_atom_long src_ts = o_bars[k % o_bars_count];
   ```
4. **Copy & Adjust Bar**:
   - It deep-copies the source bar dictionary: `t_dictionary *copied_bar_dict = dictionary_deep_copy(src_bar_dict);`
   - It calls `adjust_filled_bar_dict(copied_bar_dict, src_ts, dest_ts);`. This is a **no-op** ensuring that internal values (such as `offset`, `span`, or `absolutes`) remain **exactly unshifted**; only the main dictionary key is updated to the destination timestamp.
5. **Write to Incumbent**: It deletes any existing bar at `dest_ts` and writes the `copied_bar_dict` under the key string of `dest_ts`.
6. **Log and Output**:
   - Logs: `[Fill Pos] Copied track <name> bar <src_ts> to <dest_ts>`.
   - Outputs the new bar via `crucible_output_bar_data` to downstream outlets.
7. **Downstream Visualization Packet**: If `@visualize` is enabled, it sends a `"fill_bar"` JSON event via TCP:
   ```json
   {"event":"fill_bar","track":"<track_name>","bar":<dest_ts>,"copied_from":<src_ts>}
   ```
8. **Memory Cleanup**: Releases the copied dictionary and frees the sorted bars array.

---

### Step 3.6: Negative Growth Padding (Backward Copying)
If a track's local minimum `o_min` is strictly greater than the global minimum `song_curr_min`:
1. **Retrieve Sorted Bars**: It retrieves the track's existing bar timestamps, sorted numerically.
2. **Loop to Fill Gaps**: It loops `dest_ts` starting from `o_min - bar_length` down to `song_curr_min`, decrementing by `bar_length`:
   ```c
   for (t_atom_long dest_ts = o_min - bar_length; dest_ts >= song_curr_min; dest_ts -= bar_length) { ... }
   ```
3. **Select Source Bar (Backward modulo indexing)**: It wraps around the sorted array from end to start using a backward modulo counter:
   ```c
   long src_idx = o_bars_count - 1 - (k % o_bars_count);
   t_atom_long src_ts = o_bars[src_idx];
   ```
4. **Copy & Adjust Bar**:
   - It deep-copies the source bar dictionary.
   - Calls `adjust_filled_bar_dict` (no-op; internal values remain unshifted).
5. **Write to Incumbent**: Deletes any existing bar at `dest_ts` and writes the `copied_bar_dict`.
6. **Log and Output**:
   - Logs: `[Fill Neg] Copied track <name> bar <src_ts> to <dest_ts>`.
   - Outputs the new bar via `crucible_output_bar_data`.
7. **Downstream Visualization Packet**: If `@visualize` is enabled, it sends a `"fill_bar"` JSON event via TCP.
8. **Memory Cleanup**: Releases the copied dictionary and frees the sorted bars array.

---

## 4. Interaction with Reach Calculations

A vital aspect of the `crucible` design is that **track and song reaches are calculated unconditionally**, regardless of the `@fill` attribute status.

During `crucible_recalculate_reaches(x)`:
- The track reach is calculated as an absolute span:
  $$\text{Track Reach} = (\text{track\_max} + \text{bar\_length}) - \text{track\_min}$$
- The global song reach is calculated as:
  $$\text{Song Reach} = (\text{song\_max} + \text{bar\_length}) - \text{song\_min}$$

Because reach calculations are absolute spans, they do not depend on whether the padding/filling has physically occurred via `@fill`. When `@fill` is disabled, the reach is still measured from the absolute earliest bar to the absolute latest bar of each track/song, even if gaps exist. When `@fill` is enabled, the physical gaps are populated, making the tracks continuous.

---

## 5. Downstream Visualization Handling

### 5.1 Handshake & Packet Sending (`shared/visualize.c`)
When a `"fill_bar"` packet is generated, it goes through `visualize()` in `shared/visualize.c`.
- The packet is identified as a `"fill_bar"` event type during JSON parsing to ensure it is enqueued and sent correctly.
- The transmission uses the robust TCP socket layer designed to prevent truncation and packet loss.

### 5.2 Python Visualizer Handling (`visualizer.py`)
When `visualizer.py` receives a `"fill_bar"` event packet:
1. **Rating Synchronization**: It copies the rating of the source bar (`copied_from`) to the destination bar:
   ```python
   if src_str in state["bar_ratings"][t_str]:
       state["bar_ratings"][t_str][b_str] = state["bar_ratings"][t_str][src_str]
   ```
2. **Track Registration**: It registers the destination bar into the local track timeline array `state["tracks"][t_str]` if it is not already present, setting a dirty flag to redraw.
3. **Queue Animation Event**: It schedules a temporary animation event of type `"fill_bar"` in `state["events"]` with a specific start time and duration.

### 5.3 Pygame Rendering
During the active drawing loop, `visualizer.py` processes active `"fill_bar"` animation events:
- It calculates the column coordinates based on the destination bar timestamp shifted relative to the song start timeline.
- Rather than rendering a flashy colorful box or floating rating text, it **renders the source bar's integer index** (calculated as `src_bar / bar_length`) centered inside the filled grid cell.
- This text is rendered in a **subdued, fading gray color** `(160, 160, 160)` to visually signify that this is an automatically filled/padded bar rather than a directly won challenger bar.

---

## 6. Downstream Max MSP Outputs (The Fill Outlet)

In the standard Max MSP right-to-left outlet firing order, the outlets for `crucible` are structured as follows:
- **Outlet 0 (Index 0)**: Bar/Cell Data
- **Outlet 1 (Index 1)**: Fill Outlet (outputs `fill` whenever a song growth event is detected)
- **Outlet 2 (Index 2)**: Reach Lists / Values

Whenever a song growth event is detected (`song_grew` is true) and a challenger wins:
- If `@async` is disabled (or if executing on the main thread), the symbol `fill` is fired directly through `outlet_fill` (Outlet index 1).
- If `@async` is enabled and executing on the background thread, the output is enqueued and deferred to the main thread via:
  ```c
  defer(x, (method)crucible_defer_output, gensym("fill"), 0, NULL);
  ```
  This triggers a safe downstream system-wide notification.

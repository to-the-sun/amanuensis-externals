# Crucible `@meld` Attribute Technical Report

## Overview and Purpose

In the `crucible` object, the `@meld` attribute controls how rating updates are propagated across bars that share a common musical span in the incumbent transcript dictionary.

A **span** represents a contiguous phrase or multi-bar segment grouping. In the transcript dictionary structure, every bar dictionary entry contains a `span` key holding an array of 1-based bar timestamps (integers or symbols) that constitute the entire span segment.

When `@meld` is enabled (`@meld 1`), receiving a `replace` message that targets a bar's `rating` key calculates a new average rating across all bars in that span, updates every constituent bar in the incumbent transcript dictionary with this averaged rating, and transmits corresponding visualizer updates. When `@meld` is disabled (`@meld 0`, the default), `replace` rating updates affect only the individual targeted bar without altering other bars in the same span.

---

## Attribute Declaration and Configuration

The `@meld` attribute is declared in `crucible.h` as a member of the `t_crucible` struct:

```c
typedef struct _crucible {
    ...
    long meld;
    ...
} t_crucible;
```

In `crucible.c`, it is registered as an `onoff` box attribute:

```c
CLASS_ATTR_LONG(c, "meld", 0, t_crucible, meld);
CLASS_ATTR_STYLE_LABEL(c, "meld", 0, "onoff", "Enable Span Rating Averaging on Replace");
CLASS_ATTR_DEFAULT(c, "meld", 0, "0");
```

- **Attribute Type:** `long`
- **Style:** `onoff` (boolean toggle in Max Inspector: `0` = off, `1` = on)
- **Default Value:** `0` (disabled)

---

## Message Format and Parsing

The `@meld` logic is triggered when `crucible` receives a `replace` message in its primary inlet.

### Message Structure

```
replace <track_id>::<bar_timestamp>::<key> <value>
```

For example:
```
replace 4::15972::rating 1.072468
```

### Parsing Mechanics (`parse_selector`)

Upon receiving a `replace` message in `crucible_do_anything()`, `crucible` parses the selector string using `parse_selector()`:
1. `track`: The track identifier (e.g., `"4"`).
2. `bar`: The bar timestamp identifier (e.g., `"15972"`).
3. `key`: The target property key (e.g., `"rating"`).

If the parsed `key` is `"rating"`, the object extracts the float value from the message argument (`specified_rating = atom_getfloat(argv + 1)`) and checks the state of `x->meld`.

---

## Execution Algorithm under `@meld 1`

When `x->meld` is `1` and a `rating` replacement message arrives, `crucible` executes a two-pass algorithm on the incumbent transcript dictionary.

```
+-----------------------------------------------------------------------+
| 1. Parse Selector: track = "4", bar = "15972", key = "rating"         |
|    Specified Rating = 1.072468                                         |
+-----------------------------------------------------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------+
| 2. Retain Incumbent Dictionary & Lookup Specified Bar Dictionary       |
+-----------------------------------------------------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------+
| 3. Retrieve `span` Key from Specified Bar Dictionary                  |
|    (e.g., span = [15972, 16097, 16222, 16347])                        |
+-----------------------------------------------------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------+
| 4. First Pass: Compute Rating Sum & Count                             |
|    - For each bar in span (except specified bar):                     |
|        Retrieve existing rating from incumbent dictionary             |
|        Add to rating_sum and increment rating_count                   |
|    - For specified bar:                                               |
|        Add specified_rating to rating_sum and increment rating_count  |
|    - Compute: avg_rating = rating_sum / rating_count                  |
+-----------------------------------------------------------------------+
                                   |
                                   v
+-----------------------------------------------------------------------+
| 5. Second Pass: Update Incumbent Dictionary                           |
|    - For each bar in span:                                            |
|        Overwrite rating key with avg_rating                           |
|        If @visualize is enabled, send JSON packet:                    |
|        {"event":"replace","track":"4","bar":"...","rating":avg_rating}|
+-----------------------------------------------------------------------+
```

### Detailed Algorithmic Steps

1. **Incumbent Dictionary Retain:**
   The object retains the registered incumbent dictionary via `dictobj_findregistered_retain(x->incumbent_dict_name)`.

2. **Track and Bar Lookup:**
   `crucible` looks up the track dictionary for `track_sym`, and within it, the bar dictionary for `bar_sym`.

3. **Span Retrieval:**
   `crucible` calls `crucible_get_span_as_atomarray(specified_bar_dict)` to retrieve the array of bar timestamps belonging to the target bar's span.

4. **Pass 1 - Span Rating Averaging:**
   `crucible` iterates over every bar timestamp `b_sym` in `span_atoms`:
   - If `b_sym == bar_sym` (the target bar specified in the message), its existing dictionary rating is skipped, and `specified_rating` is added to `rating_sum`.
   - For all other bars in the span, `crucible` fetches their current rating from their respective bar dictionaries in `track_dict`, adding each to `rating_sum` and incrementing `rating_count`.
   - The averaged rating is calculated as:
     `avg_rating = rating_sum / (double)rating_count`

5. **Pass 2 - Dictionary Mutation and Visualization:**
   `crucible` iterates over all bars in the span again:
   - It updates the `rating` key of each bar in `track_dict` to `avg_rating`.
   - If `@visualize` is enabled (`x->visualize == 1`), `crucible` formats and sends a JSON packet for each bar in the span:
     `{"event":"replace","track":"4","bar":"15972","rating":avg_rating}`
     `{"event":"replace","track":"4","bar":"16097","rating":avg_rating}`
     `...`

6. **Cleanup:**
   The atomarray and dictionary references are released cleanly via `object_release()` and `dictobj_release()`.

---

## Behavior Comparison: `@meld 0` vs `@meld 1`

| Aspect | `@meld 0` (Default) | `@meld 1` (Enabled) |
| :--- | :--- | :--- |
| **Incumbent Dict Mutation** | Does not modify the incumbent dictionary on `replace` messages. | Overwrites the `rating` key of **all** bars in the span in the incumbent dictionary. |
| **Rating Calculation** | Directly uses the specified rating string/float without modifications. | Calculates the arithmetic mean of the specified rating plus all other current ratings in the span. |
| **Visualizer Packets** | Sends a single `replace` event JSON packet for the specified bar if `@visualize 1`. | Sends individual `replace` event JSON packets for **every** bar in the span with the averaged rating. |
| **Non-`rating` Keys** | Ignored by `@meld` (selectors with keys like `palette` or `offset` do not trigger meld logic). | Ignored by `@meld` (selectors with keys like `palette` or `offset` do not trigger meld logic). |

---

## Speculative Analysis: How Bars in the Same Span Can Have Different Ratings in a Transcript Dictionary

When spans are initially constructed by `buildspans` or processed during span competition in `crucible`, every bar within a single span is assigned an identical rating (derived from `lowest_mean * bars_with_mean_count` for that span).

However, in practice, a transcript dictionary may contain spans whose constituent bars possess different ratings. Below are four potential technical scenarios explaining how this discrepancy can occur:

### Scenario A: Targeted `replace` Messages Received with `@meld` Disabled (`@meld 0`)

When external objects or user patches send `replace` messages to adjust bar ratings (for example, manual rating adjustments, user feedback scoring, or automation curves), the outcome depends entirely on `@meld`:
- If `@meld` is `0` (the default setting), receiving `replace 4::15972::rating 1.5` updates only bar `15972`.
- The remaining bars in that span (e.g., `16097`, `16222`) retain their original ratings.
- Consequently, bars belonging to the exact same `span` array will now hold disparate `rating` values in the transcript dictionary.

### Scenario B: Manual or Scripted Modifications to Transcript Files

Transcript files (e.g., `sampletranscript.json` or `transcript.json`) can be modified outside of `crucible` by Python scripts, node processes, or text editors.
- If an external utility selectively updates individual bar dictionaries without recalculating or propagating ratings across the entire array listed under `span`, rating divergence occurs.
- When `crucible` subsequently loads this dictionary into memory, the mismatch persists until a melded `replace` operation or complete rebarring process is executed.

### Scenario C: Rebarring and Span Realignment Operations (`rebar`)

During a `rebar` operation (e.g., converting a track from a 125ms bar length to a 250ms bar length):
- `crucible` recalculates bar boundaries and re-groups absolute note timestamps and scores.
- If post-conversion bars are re-assembled across previous span boundaries or if partial span overlaps occur during transform passes, bars from different original spans (with different original ratings) may be merged or assigned new localized ratings before spans are finalized.

### Scenario D: Partial Span Overwriting During Competition without `@consume 1`

When a challenger span wins competition in `crucible_process_span()`:
- If `consume` is disabled (`@consume 0`), winning bars overwrite only the exact matching bar timestamps in the incumbent dictionary.
- If the winning challenger span partially overlaps an existing incumbent span, the overwritten bars receive the challenger span's rating, while the un-overwritten bars of the former incumbent span retain their old ratings while still pointing to their original `span` definition array.

---

## Summary

The `@meld` attribute in `crucible` provides automated span-wide rating synchronization upon single-bar rating replacements. By averaging new rating inputs across all bars listed in a span's metadata array, `@meld` ensures musical coherence across multi-bar segments while maintaining real-time synchronization with external visualizers.

# Investigation: Errant `0.0` Offset during Bang/Flush Routine

## Overview
This report speculates on the root causes of the `0.0` offset initialization observed in the `buildspans` object, specifically occurring around the receipt of a `bang` message.

## The Standard Bang-and-Flush Routine
When a `bang` message is received, the following sequence of events occurs within the C code:

1.  **`buildspans_bang` Activation**:
    *   Logs the start of a global flush.
    *   Scans the entire `x->building` dictionary to identify every unique `palette` currently holding data.
    *   Calls `buildspans_flush(x, palette_sym)` for each identified palette.
    *   **State Reset**: Sets `x->local_bar_length = 0` (forcing a buffer re-read on the next note).
    *   **History**: Sets `x->last_msg_type = gensym("bang")`.

2.  **`buildspans_flush` (Per Palette)**:
    *   Identifies all unique tracks within the target palette by parsing keys (e.g., `4-1000`, `5-1000`).
    *   Iterates through these tracks and triggers a final rating check and closure.

3.  **`buildspans_end_track_span` (The "Purge")**:
    *   Calculates the final span rating and outputs the span list and track data to outlets.
    *   **Dictionary Deletion**: Deletes every entry in the `x->building` dictionary associated with that specific palette and track.
    *   **Post-Closure Cleanup**: Adds the track to a deferred cleanup queue to remove obsolete offsets.

## Hypotheses for the `0.0` Offset Initialization

### 1. The "Tier 1 Vacuum" Effect
The `buildspans` object uses a two-tier hierarchy to resolve note offsets:
*   **Tier 1**: Lookup existing high-precision offsets in the dictionary for the active span.
*   **Tier 2**: Fallback to the Effective Global Offset (`current_offset` or `calc_timestamp`).

**The Scenario**: Immediately after a `bang`, the dictionary for a track is **empty**. If a new note (`list` message) arrives immediately after the `flush` (or is triggered by the flush's own output via a feedback loop in Max), Tier 1 lookup is **guaranteed to fail**. 

The object then falls back to Tier 2. if `x->current_offset` is `0.0` (the default state upon object creation or after a `clear`), and no new `offset` message has arrived to update it, the note is assigned an offset of `0.0`.

### 2. Ephemeral Auto-Initialization Race
If `x->current_offset` is `<= 0.0`, the object enters "Ephemeral Auto-Initialization" mode, where it uses the note's own `calc_timestamp` as its offset for that specific note.

**The Scenario**: If a note with a `calc_timestamp` of exactly `0.0` arrives while the object is in this state (which is the state after a `clear` or upon instantiation), it will initialize a span with an offset of `0.0`. If a `bang` happens and the next note happens to have a 0.0 timestamp (perhaps a reset signal in the user's patch), the `0.0` offset is recorded.

### 3. Re-entrancy and Feedback Loops
Max is a message-based environment. When `buildspans_end_track_span` calls `outlet_anything`, it triggers a chain of events in the Max patch.

**The Scenario**: If the user's patch responds to the "span ended" or "track number" output by sending a new note back into the object's first inlet *before the bang routine has finished*, the object may be in an inconsistent state where the dictionary is empty but the "last message" hasn't been updated to `list` yet. This re-entrant note would trigger the Tier 2 fallback described in Hypothesis 1.

### 4. Rounded Offset Grouping (The "4-0" Span)
The object groups notes into spans using the rounded value of the offset (e.g., `track-round(offset)`). 

**The Scenario**: If an offset of `0.4` is set, the span is named `4-0`. While the precision offset is `0.4`, the track-offset identifier is `4-0`. If the code incorrectly retrieves the `0` from the symbol name instead of the `0.4` from the dictionary (which can happen if Tier 1 fails), the precision `0.0` is used.

## Summary of the "Post-Flush" State
After a `bang`, the object is in a "sensitive" state:
1.  **Memory is empty** (Tier 1 will fail).
2.  **Bar length is 0** (will be re-read from buffer).
3.  **Global Offset persists**: Whatever `current_offset` was before the `bang` is still there. If it was `0.0`, the object is primed to create a `0.0` offset span upon the next note.

## Conclusion
The most likely cause is the **Tier 1 Vacuum**. A note is arriving (either concurrently or via re-entrancy) when the dictionary has been purged by the `flush`, and the object falls back to a default `0.0` global offset because the state was never explicitly updated to a valid timestamped offset.

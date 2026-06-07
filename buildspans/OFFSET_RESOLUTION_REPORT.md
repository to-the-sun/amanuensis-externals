# buildspans: Offset Resolution and Span Assignment Report

## Overview

The `buildspans` object is responsible for aggregating incoming notes into temporal "spans" (sequences of bars). Because multiple versions of a track might exist simultaneously (due to duplication or historical offsets) and the global offset may be ephemeral, the object employs a two-tier resolution system to determine which offset to use and which spans should receive a new note.

---

## 1. Global Offset Initialization

The object maintains a global `x->current_offset` which is initialized to `0.0` upon creation or after a `clear` message.

### Ephemeral Auto-Initialization
If the global `x->current_offset` is `<= 0.0`, the object applies a rule of **Ephemeral Auto-Initialization** for incoming notes:
- **Rule:** If `current_offset <= 0.0`, the `calc_timestamp` of the current note is used as the **Effective Offset** for that note's processing cycle.
- **Persistence:** This calculation does **not** modify the global `x->current_offset`. Subsequent notes will continue to use their own `calc_timestamp` as an ephemeral anchor unless a manual offset is set.

### Manual Offset Setting
When a manual offset is sent to Inlet 2:
- If the value is `> 0.0`, it becomes the persistent `x->current_offset`.
- This persistent value will be used for all subsequent notes until the object is cleared or a new manual offset is provided.

---

## 2. Span Identification (The "Versions")

When a note arrives for a specific Track ID (Inlet 3), the object identifies **all active versions** of that track for the current palette.

### How Versions are Found:
1.  **Dictionary Scan:** The object scans the `building` dictionary for any track symbols that start with the current track number (e.g., `"1-"`) and belong to the current palette.
2.  **Global Candidate:** It also constructs a "current" track symbol using the rounded version of the **Effective Offset** (e.g., if the effective offset is `1234.56`, it looks for `"1-1235"`).
3.  **Aggregation:** All unique track symbols found are added to a list. A verbose log message is emitted listing all identified candidate spans.

---

## 3. The `actual_offset` Resolution Hierarchy

For each identified span version, the object must determine the precise `double` offset to use for bar assignment. It follows this two-tier priority:

### Tier 1: Dictionary Lookup (High Precision Preservation)
- **Logic:** Search for the first available bar within the identified track that has an `"offset"` property.
- **Rationale:** If a span already exists in memory, it carries its own high-precision anchor. Using this preserved value ensures that all notes within that span are aligned to the exact same temporal grid, even if the global offset has since moved or was ephemeral.

### Tier 2: Effective Global Offset (New Span Initialization)
- **Logic:** If no offset is found in the dictionary, the object uses the **Effective Offset** (either the persistent `x->current_offset` or the ephemeral `calc_timestamp`).
- **Rationale:** This is used for the "active" span that is just beginning. It provides full `double` precision for the initialization of a new span.

---

## 4. Bar Assignment Logic

Once the `actual_offset` is resolved, the note is passed to `buildspans_process_and_add_note`.

### Relative Calculation:
The object calculates the position of the note relative to the span's start:
`relative_timestamp = calc_timestamp - actual_offset + loop_start`

### Bar Quantization:
The note is then assigned to a bar by rounding down to the nearest multiple of the current `bar_length`:
`bar_timestamp = floor(relative_timestamp / bar_length) * bar_length`

### Duplication Interaction:
When the global offset is manually updated in a way that triggers duplication:
1.  Existing spans remain in the dictionary with their original offsets.
2.  New spans are created at the new rounded offset.
3.  Notes from the old spans are copied into the new spans.
4.  Subsequent incoming notes are identified as belonging to **both** the old and new versions, maintaining continuity across the transition.

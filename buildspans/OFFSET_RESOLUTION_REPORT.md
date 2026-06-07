# buildspans: Offset Resolution and Span Assignment Report

## Overview

The `buildspans` object is responsible for aggregating incoming notes into temporal "spans" (sequences of bars). Because the global offset can change or be auto-initialized, and because multiple versions of a track might exist simultaneously (due to duplication or historical offsets), the object employs a robust resolution system to determine which offset to use and which spans should receive a new note.

---

## 1. Global Offset Initialization

The object maintains a global `x->current_offset` and an `x->offset_fixed` flag.

### Auto-Initialization
If the object is in a fresh state (`offset_fixed == 0` and `current_offset <= 0.0`), the very first note received will trigger auto-initialization.
- **Trigger:** The first note arrives via `buildspans_list`.
- **Action:** `x->current_offset` is set to the `calc_timestamp` of that note.
- **Result:** This anchor ensures that relative bar calculations are aligned with the start of the performance if no manual offset was provided.

### Manual Offset Setting
When a manual offset is sent to Inlet 2:
- If the value is `> 0.0`, `x->offset_fixed` is set to `1`. This prevents further auto-initialization.
- If the value is `<= 0.0`, `x->offset_fixed` is reset to `0`, allowing the next note to re-initialize the global offset.

---

## 2. Span Identification (The "Versions")

When a note arrives for a specific Track ID (Inlet 3), the object does not just add it to one span. It identifies **all active versions** of that track for the current palette.

### How Versions are Found:
1.  **Dictionary Scan:** The object scans the `building` dictionary for any track symbols that start with the current track number (e.g., `"1-"`) and belong to the current palette.
2.  **Global Candidate:** It also constructs a "current" track symbol using the rounded version of the global `x->current_offset` (e.g., if offset is `1234.56`, it looks for `"1-1235"`).
3.  **Aggregation:** All unique track symbols found are added to a list. This allows a single note to be multi-cast into multiple versions of the same track if they are currently being built or maintained.

---

## 3. The `actual_offset` Resolution Hierarchy

For each identified span version, the object must determine the precise `double` offset to use for bar assignment. It follows this priority:

### Tier 1: Dictionary Lookup (High Precision)
- **Logic:** Search all bars within the identified track for an `"offset"` property.
- **Rationale:** If a span already exists in memory, it carries its own high-precision anchor. Using this preserved value ensures that all notes within that span are aligned to the exact same temporal grid, even if the global offset has since moved.

### Tier 2: Global Current Offset (New Span Initialization)
- **Logic:** If the track symbol matches the current global candidate and was not found in the dictionary.
- **Rationale:** This is used for the "active" span that is just beginning. It provides the full `double` precision of the current global offset (including decimals) for the very first note added to a new span.

### Tier 3: Symbolic Parsing (Robust Fallback)
- **Logic:** Parse the integer offset from the track's symbol name (e.g., extract `1235` from `"1-1235"`).
- **Rationale:** If a track exists in the dictionary but somehow lost its property entries, or if it's a historical track being referenced, the symbol name provides a reliable (though integer-rounded) fallback.

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
1.  Existing spans are effectively "frozen" at their current rounded offset.
2.  New spans are created at the new rounded offset.
3.  Notes from the old spans are copied into the new spans.
4.  Subsequent incoming notes will then be identified as belonging to **both** the old and new versions (via the Tier 2 scan) until the old spans are eventually flushed or cleaned up.

---

## Summary of Resolution Priority

1.  **Identify Spans:** Find all tracks in the dictionary matching `[TrackID]-*` + Current Palette.
2.  **Add Global:** Include `[TrackID]-[Round(x->current_offset)]` as a candidate.
3.  **Resolve Offset:** For each candidate, use **Dictionary > Global > Symbol** to find the `actual_offset`.
4.  **Process:** Calculate the bar for each span and store the note data.

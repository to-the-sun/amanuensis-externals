# Hierarchical Migration Strategy for BuildSpans

To improve performance and eliminate the $O(N^2)$ duplication bottleneck without causing the crashes observed in previous attempts, this migration will follow a strict "small steps" approach. Each phase results in a fully functional object.

## Phase 1: The Active Registry (Preparation)
**Goal:** Replace linear scans for "discovery" with a dedicated, lightweight registry.

*   **Action:** Add a `t_dictionary *registry` to the `t_buildspans` struct.
*   **Logic:** When a note is added to a new track/palette, register that track/palette combination in the registry.
*   **Performance Gain:** Functions like `buildspans_do_offset` (duplication) and `buildspans_flush` no longer need to iterate over every key in the main `building` dictionary to find active tracks. They simply look at the registry.
*   **Stability:** This is performance-neutral for storage but reduces CPU load for discovery. The core `building` dictionary remains flat.

## Phase 2: Interface Abstraction
**Goal:** Decouple the logic from the flat string key format.

*   **Action:** Create a suite of internal helper functions for dictionary access:
    *   `buildspans_get_bar_atom(palette, track, bar, property)`
    *   `buildspans_set_bar_atom(palette, track, bar, property, atom)`
*   **Logic:** Refactor all existing code to use these helpers instead of manual `snprintf` and `dictionary_get...` calls.
*   **Stability:** This makes the subsequent "Structural Pivots" much safer, as the change in storage format will only need to be updated inside these few helper functions.

## Phase 3: Palette-Level Nesting
**Goal:** Partition the data by Palette.

*   **Action:** Update the storage logic so `x->building` contains one sub-dictionary per Palette.
*   **Structure:** `building` -> `{palette_name}` -> `{track::bar::property}` (Flat keys inside).
*   **Performance Gain:** Operations are now scoped to a single palette, reducing the search space for dictionary lookups.
*   **Stability:** Verification is easy: only the palette-level parsing is removed from the flat keys.

## Phase 4: Track-Level Nesting & $O(1)$ Duplication
**Goal:** Implement the primary performance fix for duplication.

*   **Action:** Update the storage logic so each Palette dictionary contains sub-dictionaries for each Track.
*   **Structure:** `palette_dict` -> `{track_id}` -> `{bar::property}`.
*   **Performance Gain:** Duplication (`buildspans_do_offset`) no longer requires copying individual notes. It simply clones the target track's dictionary using `dictionary_clone()`.
*   **Stability:** This is the most complex step. By keeping the `bar::property` part flat for now, we minimize the depth of the change.

## Phase 5: Full Hierarchy (Removing String Parsing)
**Goal:** Eliminate all `snprintf` and string parsing overhead.

*   **Action:** Complete the nesting: `track_dict` -> `{bar_timestamp}` -> `{property_name}`.
*   **Performance Gain:** Zero string manipulation during note entry. Properties like `mean`, `offset`, and `rating` are direct lookups in a tiny bar-level dictionary.
*   **Stability:** At this point, `parse_hierarchical_key` and `generate_hierarchical_key` can be deleted from the codebase.

## Phase 6: Advanced Concurrency (Optional)
**Goal:** Maximize audio thread safety.

*   **Action:** Implement double-buffering for the hierarchical dictionaries.
*   **Logic:** The background thread builds a new "frozen" version of the hierarchy and swaps the pointer with the main thread in a single atomic operation.
*   **Performance Gain:** Zero mutex contention for the high-priority thread.

## Stability & Ownership Guardrails (Lessons from Phase 0)

During initial leak-fixing attempts, it was discovered that the Max SDK ownership model for dictionaries is particularly treacherous. To avoid crashes during the hierarchical transition, the following rules must be enforced:

1.  **Container Ownership Hand-off:** Functions like `dictionary_appenddictionary` and `dictionary_appendatom` (with `A_OBJ`) typically assume ownership of the reference passed to them *without* necessarily incrementing the reference count in all SDK versions. When moving to Phase 3 and 4, never `object_release` a sub-dictionary immediately after appending it unless the specific SDK documentation for that function confirms a retain.
2.  **Weak vs. Strong Retrieval:** `dictionary_getdictionary` and `dictionary_getatomarray` return "weak" pointers (borrowed references). Releasing these pointers will cause immediate crashes during playback. The migration must use a "retain-on-get" strategy only if the object needs to outlive the parent dictionary's immediate scope.
3.  **NULL-Safe Traversal:** Hierarchical lookups (e.g., `building -> palette -> track -> bar`) are prone to deep NULL dereferences. Every level of the nesting must be checked before proceeding to the next. Phase 2 (Interface Abstraction) is critical for centralizing these checks.
4.  **The "Release-at-End" Pattern:** For objects that are genuinely owned (created with `_new` or `_clone`), use boolean flags to track ownership and perform `object_release` at the very end of the function, after all logging and back-propagation are complete. This prevents the "use-after-free" crashes experienced during the initial audit.

# buildSpans: Duplication Performance and Audio Thread Interruptions

This report analyzes the performance of the `buildSpans` object, specifically during span duplication events triggered by `offset` messages. It identifies why audio thread interruptions occur even when the `@async` attribute is enabled and proposes architectural improvements to resolve these issues.

## 1. The Async Threading Bug: "Incomplete Offloading"

The primary reason for audio interruptions when `@async` is enabled is a logic error in how tasks are enqueued to the background worker.

### The Problematic Logic
Throughout `buildspans.c` (and similarly in `crucible.c`), enqueuing logic follows this pattern:

```c
if (x->async && x->worker && systhread_ismainthread()) {
    async_worker_enqueue(...);
    return;
}
```

This logic says: "If async is enabled AND we are currently on the **Main (GUI) Thread**, move the task to the background."

### The Audio Thread Impact
In a typical Max patch, note data and offset updates often originate from the **Scheduler Thread** or the **Audio Thread** (via `bang~` or `edge~`).
- When an `offset` update arrives from the Audio/Scheduler thread, `systhread_ismainthread()` returns **false**.
- The object then skips the `async_worker_enqueue` block and executes the heavy duplication logic **immediately on the high-priority thread**.

**Result**: The audio thread is forced to perform a synchronous, multi-millisecond dictionary scan, resulting in an audible glitch (dropout).

---

## 2. Algorithmic Bottlenecks in Duplication

Even if successfully moved to a background thread, the current duplication algorithm is inefficient, scaling poorly as the number of active spans increases.

### $O(N^2)$ to $O(N^3)$ Complexity
The `buildspans_do_offset` function performs multiple full-dictionary scans:
1.  **Discontiguity Check**: Scans every key in the dictionary to find unique palette-track pairs, then performs a sub-scan for each.
2.  **Gather Phase**:
    - Scans the entire dictionary to identify unique track numbers.
    - For *each* unique track number, it scans the dictionary *again* to check if a target exists.
    - For *each* unique track number, it scans the dictionary *a third time* to gather notes from the source span.
3.  **Processing Phase**: For every note gathered, it calls `buildspans_process_and_add_note`, which itself performs multiple dictionary scans to find the most recent bar and update the span array.

In a patch with hundreds of active bars, this results in millions of string comparisons and key lookups per duplication event.

### Memory Allocation Overhead
The helper `parse_hierarchical_key` is the object's hottest code path. For every key inspected during a scan:
- It performs 3 `strstr` calls.
- It performs 4 `sysmem_newptr` allocations.
- It performs 4 string copies.
- The caller then performs 4 `sysmem_freeptr` calls.

During a duplication event involving 1,000 keys and multiple scan passes, the object may be performing over 20,000 heap allocations/deallocations synchronously.

---

## 3. Speculative Synthesis: The "Grip" Effect

When a long span is being duplicated:
1.  A new `offset` message arrives on the audio thread.
2.  Because it's not the main thread, `buildspans` "grips" the audio thread.
3.  The CPU is pegged for several milliseconds performing $O(N^2)$ string-heavy work.
4.  The audio buffer pointer fails to advance in time, causing the interruption.

---

## 4. Proposed Technical Solutions

To alleviate these interruptions and make the object production-ready for large-scale patches, the following changes are recommended:

### A. Fix the Threading Gating
The gating logic should be changed to ensure work is offloaded *unless* it specifically needs to be synchronous.
```c
if (x->async && x->worker) {
    // Enqueue if we are in the Audio/Scheduler thread.
    // Optionally, if we are already in the background worker thread, execute synchronously.
    if (!systhread_ismainthread()) {
        async_worker_enqueue(...);
        return;
    }
}
```

### B. Single-Pass Dictionary Scanning
Instead of multiple nested scans, the duplication logic should be refactored into a single pass:
1.  Scan the dictionary once.
2.  Categorize every key found into a temporary local structure (e.g., a linked list of "source bars" and "target existence flags").
3.  Execute the duplication based on this pre-sorted local context.

### C. Optimize Key Parsing (The "Token" Approach)
Instead of allocating four new strings for every key check, `parse_hierarchical_key` should be modified to return "tokens" (pointers into the original string) or use a static/thread-local scratch buffer to avoid heap thrashing.

### D. Hierarchical Dictionary Structure
Moving from a flat `building` dictionary with "::" separators to a truly nested `t_dictionary` structure (Palette -> Track -> Bar -> Property) would allow the object to use Max's internal hashing more effectively, reducing the complexity of most lookups from $O(N)$ to $O(1)$.

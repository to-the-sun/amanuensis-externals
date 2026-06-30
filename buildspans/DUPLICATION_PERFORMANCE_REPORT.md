# buildSpans: Duplication Performance and Audio Thread Interruptions

This report analyzes the performance of the `buildSpans` object, specifically during span duplication events triggered by `offset` messages. It identifies why audio thread interruptions occur even when the `@async` attribute is enabled and proposes architectural improvements to resolve these issues.

## 1. The Async Threading Bug: "Incomplete Offloading"

The primary reason for audio interruptions when `@async` is enabled is a logic error in how tasks are enqueued to the background worker.

### The Problematic Logic
Previously, enqueuing logic followed this pattern:

```c
if (x->async && x->worker && systhread_ismainthread()) {
    async_worker_enqueue(...);
    return;
}
```

This logic only moved the task to the background if it originated from the **Main (GUI) Thread**.

### The Audio Thread Impact
Offset updates often originate from the **Scheduler Thread** or the **Audio Thread**. In these cases, `systhread_ismainthread()` returns **false**, causing the object to skip the background queue and execute heavy duplication logic **immediately on the high-priority thread**.

**Fix**: The logic has been updated to gate on `!systhread_ismainthread()`, ensuring that Audio/Scheduler thread tasks are properly offloaded.

---

## 2. Algorithmic Bottlenecks in Duplication

Even when offloaded, the current duplication algorithm is inefficient.

### $O(N^2)$ to $O(N^3)$ Complexity
The duplication process involves multiple full-dictionary scans. In a patch with hundreds of active bars, this results in millions of string comparisons and key lookups per event.

### Memory Allocation Overhead
The helper `parse_hierarchical_key` previously allocated four new strings for every key check via `sysmem_newptr`. During a large duplication event, this resulted in thousands of synchronous heap allocations/deallocations, thrashing the memory manager.

---

## 3. Implemented and Proposed Optimizations

### Stack-Based Key Parsing (Implemented)
`parse_hierarchical_key` has been refactored to use a fixed scratch buffer on the stack for its internal string manipulation, significantly reducing heap pressure during dictionary scans.

### Proposed: Hierarchical Dictionary Structure
Transitioning from a flat dictionary (using "::" separators) to a nested `t_dictionary` structure (Palette -> Track -> Bar -> Property) would allow for $O(1)$ lookups using Max's internal hashing, eliminating the need for linear scans during duplication.

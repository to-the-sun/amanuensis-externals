# BuildSpans Duplication Performance Report

## The "Audio Thread Grip" Phenomenon
Even with `@async 1` enabled, `buildSpans` was previously observed causing audio thread interruptions. This investigation revealed that while the object supported asynchronous processing, the **gating logic** was only offloading tasks if they originated from the Max Main thread.

In many musical patches, `offset`, `note`, and `bang` messages are delivered via the **Scheduler** or **Audio** threads (high-priority). Because the previous logic only checked for the Main thread, these high-priority threads were forced to execute the expensive duplication and dictionary modification logic synchronously, leading to dropped audio frames.

## Architectural Improvements

### 1. Robust Async Offloading (Implemented)
The threading logic has been updated to offload work to a background thread when `@async` is enabled, regardless of whether the call originates from the **Main**, **Audio**, or **Scheduler** thread. This ensures that the high-priority audio engine is never blocked by expensive duplication or dictionary modification tasks.
- **Recursion Safety:** A new `async_worker_is_worker_thread()` check ensures that the background worker itself doesn't try to re-enqueue its own work, which prevents infinite loops and deadlocks.
- **Entry Point Consistency:** All primary entry points (`offset`, `list`, `bang`, `track`, `clear`, `local_bar_length`) have been unified to use this robust offloading strategy.

### 2. Hierarchical Dictionary Structure
Previously, `buildSpans` used a "flat" dictionary key structure (e.g., `palette::track::bar::property`). This forced the duplication process to perform linear scans ($O(N^2)$ complexity) to find and copy keys belonging to a specific track.

We have refactored the internal storage to a **Hierarchical Model**:
- **Palette** -> **Track** -> **Bar** -> **Property**
- **Efficiency Gain:** Duplicating a track now involves retrieving a single dictionary branch and cloning it. This reduces the complexity from $O(N^2)$ to $O(N)$ for duplication and $O(1)$ for property lookups.

### 3. Allocation Reduction
String parsing and key generation have been optimized to reduce temporary heap allocations. By leveraging nested dictionaries, we avoid the repeated `sprintf` calls previously required to generate long flattened keys.

## Speculative Future Work
- **Lock-Free Dictionary Swapping:** Implementing a double-buffering scheme for the hierarchical dictionaries could further reduce the time the high-priority thread spends waiting for a mutex during the `@async` handoff.
- **Reference Counting:** If multiple tracks share identical bar data, using a reference-counted dictionary structure (Max `t_dictionary` natively supports some of this) could make duplication near-instantaneous ($O(1)$).

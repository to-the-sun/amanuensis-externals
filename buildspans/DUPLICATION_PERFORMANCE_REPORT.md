# BuildSpans Duplication Performance Report

## The "Audio Thread Grip" Phenomenon
Even with `@async 1` enabled, `buildSpans` was previously observed causing audio thread interruptions. This investigation revealed that while the object supported asynchronous processing, the **gating logic** was only offloading tasks if they originated from the Max Main thread.

In many musical patches, `offset`, `note`, and `bang` messages are delivered via the **Scheduler** or **Audio** threads (high-priority). Because the previous logic only checked for the Main thread, these high-priority threads were forced to execute the expensive duplication and dictionary modification logic synchronously, leading to dropped audio frames.

## Architectural Improvements

*Currently focused on structural integrity and preparatory work for hierarchical migration.*

## Speculative Future Work

Detailed implementation steps for the following items can be found in [HIERARCHICAL_MIGRATION_STRATEGY.md](HIERARCHICAL_MIGRATION_STRATEGY.md).

### 1. Robust Async Offloading (Pending Re-implementation)
This feature was previously implemented to offload work regardless of the originating thread (Main, Audio, or Scheduler), but was **rolled back** due to instability in future versions.
The threading logic should be updated to offload work to a background thread when `@async` is enabled, regardless of whether the call originates from the **Main**, **Audio**, or **Scheduler** thread. This ensures that the high-priority audio engine is never blocked by expensive duplication or dictionary modification tasks.
- **Recursion Safety:** Re-implementing the `async_worker_is_worker_thread()` check to prevent infinite loops.
- **Entry Point Consistency:** Unifying all entry points (`offset`, `list`, `bang`, `track`, `clear`, `local_bar_length`) under a single offloading strategy.
- **Ecosystem Coordination:** In a complex patch involving `rebar` and `@bind`, the re-implementation must ensure that all objects share a single `t_async_worker` context where appropriate, and that pointer handoffs between `buildspans` and `crucible` are thread-safe and non-blocking.

### 2. Hierarchical Dictionary Structure (Not Yet Implemented)
Previously, `buildSpans` used a "flat" dictionary key structure (e.g., `palette::track::bar::property`). This forced the duplication process to perform linear scans ($O(N^2)$ complexity) to find and copy keys belonging to a specific track.

A proposal exists to refactor the internal storage to a **Hierarchical Model**:
- **Palette** -> **Track** -> **Bar** -> **Property**
- **Efficiency Gain:** Duplicating a track would involve retrieving a single dictionary branch and cloning it. This would reduce the complexity from $O(N^2)$ to $O(N)$ for duplication and $O(1)$ for property lookups.

### 3. Allocation Reduction (Not Yet Implemented)
String parsing and key generation could be optimized to reduce temporary heap allocations. By leveraging nested dictionaries, we would avoid the repeated `sprintf` calls currently required to generate long flattened keys.

- **Lock-Free Dictionary Swapping:** Implementing a double-buffering scheme for the hierarchical dictionaries could further reduce the time the high-priority thread spends waiting for a mutex during the `@async` handoff.
- **Reference Counting:** If multiple tracks share identical bar data, using a reference-counted dictionary structure (Max `t_dictionary` natively supports some of this) could make duplication near-instantaneous ($O(1)$).

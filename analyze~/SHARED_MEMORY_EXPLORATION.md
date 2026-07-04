# Shared Memory Exploration: Cumulative Transience Analysis

This report explores the technical feasibility, implications, and architecture for a shared memory space for the 5-second cumulative history buffer in the `analyze~` Max object.

## Overview

In the current implementation of `cumulative_transience.c`, each `TransientAnalyzer` instance maintains its own `accumulated_buffer[5001]`. This 5-second buffer (representing a -5s to 0s window relative to a peak) tracks the sum of all normalized peak snapshots for every peak detected within the last 15 seconds. When multiple `analyze~` objects are used (e.g., one for Kick, one for Snare, one for Synth), they are currently "blind" to each other's transience.

The proposal is to allow multiple `analyze~` instances to contribute to and read from a shared `accumulated_buffer`.

## Technical Feasibility

### Architecture
Implementing this would require moving the `accumulated_buffer` from the `TransientAnalyzer` struct to a global, reference-counted structure.

```c
typedef struct {
    double accumulated_buffer[BUFFER_LEN];
    int ref_count;
    t_critical lock;
    // ... possibly other shared metrics
} SharedTransientBuffer;

static SharedTransientBuffer* g_shared_buffers[MAX_GROUPS];
```

### Implementation Steps
1. **Global Registry**: A static array or dictionary in the C core to manage shared buffers by name or ID.
2. **Reference Counting**: The first `analyze~` object to request a specific ID creates the buffer; subsequent objects increment a reference count.
3. **Modified Lifecycle**:
   - `analyzer_create` would take a "buffer group ID".
   - `analyzer_process_peak` would update the shared buffer instead of an internal one.
   - `analyzer_cleanup_snapshots` would subtract snapshots from the shared buffer.

## Benefits

1. **Global Transience Awareness**: This is the primary benefit. If a "Kick" object generates a high-energy transient, the "Synth" object will immediately see an increased cumulative density in its buffer. This allows the "Rating" and "Score" of one object to be context-aware of the rest of the mix.
2. **Unified Rating System**: The "rating" metric (the average total score of peaks) would become a global measure of how "crowded" the transience space is across all shared streams.
3. **Grouping Logic**: By using IDs (e.g., `@group drums`), users could choose which elements of their mix compete for transience "space" and which remain independent.

## Difficulties and Risks

### 1. Thread Safety
In Max, `analyze~` objects perform analysis in an asynchronous worker thread. If multiple objects share a buffer, they must synchronize access.
- **Solution**: Use `t_critical` to protect the `accumulated_buffer` during addition/subtraction.
- **Performance Impact**: Since updates happen at 100ms intervals (or when peaks are detected), the lock contention should be minimal compared to the signal processing cost.

### 2. Snapshot Management
Even with a shared buffer, each `TransientAnalyzer` **must** maintain its own `SnapshotEntry` queue. This is because an instance needs to know exactly what *it* contributed to the shared pool so it can subtract that contribution 15 seconds later.

### 3. Normalization Discrepancies
The snapshots are currently normalized by the `max_peak` seen by the *local* analyzer.
- **The Problem**: If a loud Kick contributes a snapshot normalized to its 1.0 peak, and a quiet Shaker contributes a snapshot normalized to its 0.1 peak (but both use their local `max_peak`), their contributions are weighted equally in the shared buffer.
- **Speculation**: This might actually be desirable (all transients are equal once normalized), or it might require a shared `max_peak` to maintain relative weighting.

### 4. Initialization Order
The `buffer_times` (the -5s to 0s time axis) must be perfectly synchronized across all objects sharing a buffer. Since `analyze~` objects are instantiated at different times, the global "0ms" point must be consistent.

## Exact Functionality Concept

### The "Add-Subtract" Logic
- **Peak Detected**: `Instance A` detects a peak. It generates a 5-second `snapshot` and **adds** it to the `SharedBuffer->accumulated_buffer`.
- **Metric Update**: `Instance B` calculates its metrics. It reads the current state of `SharedBuffer->accumulated_buffer`. It sees the peak from `Instance A` and its own history.
- **Cleanup**: 15 seconds later, `Instance A` calls `cleanup_snapshots`. it **subtracts** its original `snapshot` from the `SharedBuffer->accumulated_buffer`.

### User Interface in Max
```maxref
analyze~ @group drums
```
- Objects with the same `@group` name share a buffer.
- Objects with no group name (or unique names) behave as they do now (private buffers).

## Conclusion

Sharing the cumulative history buffer is technically feasible and provides a powerful mechanism for multi-stream transience analysis. The main challenges are robust thread synchronization and ensuring that the normalization logic remains musically meaningful when streams of different intensities are mixed in the shared memory space.

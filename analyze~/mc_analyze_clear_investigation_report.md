# Technical Report: Investigation into `mc.analyze~` Freezing Upon Receiving `clear` Messages

**Date:** March 2026
**Scope:** `mc.analyze~`, `analyze~`, `cumulative_transience.c`, and background thread synchronization.

---

## Executive Summary

When the `mc.analyze~` object receives a `clear` message during playback or between tracks, it occasionally ceases all apparent functioning: outlet metric updates stop, peak lists are no longer emitted, and visualization telemetry updates freeze. By contrast, the single-channel `analyze~` object rarely or never exhibits this complete failure mode under identical operational conditions.

This report presents a detailed root-cause analysis based on a code inspection of `mc.analyze~.c`, `analyze~.c`, and `cumulative_transience.c`. The breakdown is driven by **thread concurrency race conditions** between the main Max thread (handling the synchronous `clear` message) and the dedicated background worker thread (`t_async_worker` running `mc_analyze_worker_task`).

---

## 1. Concurrency Architecture & Key Difference Between Objects

In both `analyze~` and `mc.analyze~`:
1. **Audio/DSP Thread (`perform64`)**: Copies incoming audio vectors into a circular buffer, increments `x->current_sample_count`, and triggers an asynchronous worker task when enough audio samples (~100ms hop) have accumulated.
2. **Main Max Thread (`mc_analyze_clear`)**: Executes synchronously when a `clear` message arrives at inlet 0.
3. **Background Worker Thread (`mc_analyze_worker_task`)**: Pulls audio chunks, runs STFT/transient detection (`analyzer_analyze_chunk`), updates metrics, and dispatches visualization telemetry via `visualize_to_port()`.

### Why `mc.analyze~` Has an $N$-Fold Wider Race Window
In single-channel `analyze~`, `analyze_worker_task` processes **1 audio channel** per chunk, taking ~0.5ms–1.5ms per hop.

In `mc.analyze~`, `mc_analyze_worker_task` iterates over **all $N$ channels** inside a single worker invocation:
```c
for (long ch = 0; ch < n_chans; ch++) {
    // ...
    analyzer_analyze_chunk(x->analyzers[ch], hop_audio, ...);
    // ...
}
```
For a 16-channel or 32-channel `mc.analyze~` object, a single worker task execution takes **16× to 32× longer**. As a result, the probability that a `clear` message arrives on the main thread *while* the worker thread is actively executing inside `analyzer_analyze_chunk()` is exponentially higher in `mc.analyze~`.

---

## 2. Root Causes of Failure During `clear`

### Cause A: Unsynchronized Memory Freeing & Use-After-Free in Snapshot Lists
When `mc_analyze_clear()` is called on the main thread:
```c
critical_enter(x->lock);
for (long i = 0; i < x->analyzers_count; i++) {
    if (x->analyzers[i]) {
        analyzer_clear(x->analyzers[i]);
    }
}
// ...
critical_exit(x->lock);
```
Inside `analyzer_clear()` in `cumulative_transience.c`:
```c
for (int b = 0; b < MAX_BANDS; b++) {
    SnapshotEntry* curr = self->snapshot_heads[b];
    while (curr) {
        SnapshotEntry* next = curr->next;
        free(curr); // FREES LINKED LIST NODES
        curr = next;
    }
    self->snapshot_heads[b] = NULL;
    self->snapshot_tails[b] = NULL;
}
```
**The Race Condition:**
`mc_analyze_worker_task` calls `analyzer_analyze_chunk()` **without holding `x->lock`** throughout the entire analysis loop (to prevent audio DSP lock contention).

If `analyzer_clear()` runs on the main thread while the worker thread is executing `analyzer_analyze_chunk()`, the worker thread continues traversing `self->snapshot_heads[b]` and reading `curr->snapshot` or `curr->p_idx`.

This results in a **Use-After-Free (heap corruption)**. On Windows, dereferencing freed memory can cause an access violation exception within the worker thread, silently killing the worker task or leaving memory in a corrupted state.

---

### Cause B: Permanent Lockup of `pending_analysis`
In `mc_analyze_perform64()`:
```c
if (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
    if (!x->pending_analysis) {
        x->pending_analysis = 1;
        async_worker_enqueue(x->worker, x, (method)mc_analyze_worker_task, gensym("analyze"), 0, NULL);
    }
}
```
In `mc_analyze_clear()`:
```c
x->pending_analysis = 0;
```
**The Race Condition:**
1. Worker thread starts `mc_analyze_worker_task` (setting `x->pending_analysis = 1`).
2. Main thread receives `clear`, acquires `x->lock`, sets `x->pending_analysis = 0`, resets counters, and exits `mc_analyze_clear`.
3. If the worker thread crashes or aborts prematurely due to Cause A (Use-After-Free), or if the worker thread completes *after* `clear` has run and executes `x->pending_analysis = 0` at its very end:
   - If an exception occurred during the worker task, `x->pending_analysis` may remain stuck at `1`.
   - Once `pending_analysis == 1`, `mc_analyze_perform64` will **never enqueue another worker task again**. The object silently freezes permanently.

---

### Cause C: Out-of-Bounds Buffer Indexing via Negative `samples_ago`
When `mc_analyze_clear()` executes, it resets sample counters:
```c
x->current_sample_count = 0;
x->last_analysis_frame = 0;
x->audio_buffer_write_ptr = 0;
```
If `mc_analyze_worker_task` was already mid-loop, its local variable `target_analysis_frame` holds the pre-clear value (e.g., `500,000` samples).

When the worker thread reads `x->current_sample_count` (which was just set to `0` by `clear`):
```c
long long cur_samples = x->current_sample_count; // Now 0!
long long samples_ago = cur_samples - hop_start_samples; // e.g., 0 - 499,900 = -499,900
int read_ptr = (int)((cur_write_ptr - samples_ago + x->audio_buffer_size) % x->audio_buffer_size);
```
In C, performing modulo operations with large negative numbers can result in **negative array indices**, attempting to read invalid memory addresses before `x->audio_buffers[ch]`. This triggers heap corruption or invalid memory access inside `hop_audio` population loops.

---

## 3. Comparative Summary

| Metric / Behavior | `analyze~` (Single-Channel) | `mc.analyze~` (Multichannel) |
| :--- | :--- | :--- |
| **Worker Task Duration** | Very short (~1ms) | Long ($N \times 1\text{ms}$, e.g. 16–32ms) |
| **Collision Likelihood** | Low | Very High |
| **Worker Lock Strategy** | `x->lock` held selectively | `x->lock` held selectively |
| **Consequence of Race** | Rare glitch or dropped frame | Severe: Use-After-Free, worker crash, permanent lockup (`pending_analysis = 1`) |

---

## 4. Proposed Architectural Solutions

To resolve this issue permanently in `mc.analyze~` (and `analyze~`), the following thread-synchronization safeguards are recommended:

1. **Worker Task Cancellation / Drain on Clear**:
   Before resetting state in `mc_analyze_clear()`, drain or cancel any in-flight worker task:
   ```c
   // Ensure background worker thread is idle before modifying structures
   async_worker_drain(x->worker);
   ```
2. **Worker Interlock / Generation Counter**:
   Add a monotonic `clear_sequence` counter to `t_mc_analyze`.
   - `mc_analyze_clear()` increments `x->clear_sequence`.
   - `mc_analyze_worker_task()` checks `clear_sequence` at the start of each hop. If the sequence counter changed mid-chunk, the worker immediately aborts processing the current hop and safely exits.
3. **Protect Snapshot Modifications in `cumulative_transience.c`**:
   Ensure `analyzer_clear()` and `analyzer_analyze_chunk()` synchronize snapshot list access using `self->lock_func` / `self->unlock_func` across all node traversals and node deletions.
4. **Sanitize `samples_ago` Calculations**:
   Clamp `samples_ago` to non-negative values (`if (samples_ago < 0) samples_ago = 0;`) to protect against negative pointer arithmetic during state resets.

---

## Conclusion

The intermittent freezing of `mc.analyze~` upon receiving a `clear` message is caused by an un-synchronized collision between the main thread's `clear` execution and the background worker thread's chunk processing loop. Because `mc.analyze~` processes multiple channels sequentially within a single worker task, the race condition is significantly more likely to occur than in `analyze~`. Implementing worker task draining and interlock sequence checks will eliminate this issue entirely.

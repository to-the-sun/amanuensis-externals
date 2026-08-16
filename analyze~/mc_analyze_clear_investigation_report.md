# Technical Report: Investigation into `mc.analyze~` Freezing Upon Receiving `clear` Messages

**Date:** March 2026
**Scope:** `mc.analyze~`, `analyze~`, `cumulative_transience.c`, and background thread synchronization.

---

## Executive Summary

When the `mc.analyze~` object receives a `clear` message during playback or between tracks, it occasionally ceases all apparent functioning: outlet metric updates stop, peak lists are no longer emitted, and visualization telemetry updates freeze. By contrast, the single-channel `analyze~` object rarely or never exhibits this complete failure mode under identical operational conditions.

Empirical testing reveals a striking symptom: after receiving a `clear` message mid-song, the object may freeze entirely for several minutes before **mysteriously resuming normal operation on its own** without user intervention.

This report presents a detailed root-cause analysis based on code inspection of `mc.analyze~.c`, `analyze~.c`, and `cumulative_transience.c`. The breakdown is driven by **thread concurrency race conditions** and **sample-counter desynchronization** between the main Max thread (handling the synchronous `clear` message) and the dedicated background worker thread (`t_async_worker` running `mc_analyze_worker_task`).

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

### Cause A: Sample Counter Desynchronization & The "Multi-Minute Silent Gap"
In `mc_analyze_perform64()`, worker task triggering depends on a sample threshold:
```c
if (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
    if (!x->pending_analysis) {
        x->pending_analysis = 1;
        async_worker_enqueue(x->worker, x, (method)mc_analyze_worker_task, gensym("analyze"), 0, NULL);
    }
}
```
When `mc_analyze_clear()` executes on the main thread:
```c
x->current_sample_count = 0;
x->last_analysis_frame = 0;
```
**The Desynchronization Race:**
1. Suppose a song has been playing for 2 minutes (sample count = $2 \times 60 \times 44,100 = 5,292,000$ samples).
2. The worker thread enters `mc_analyze_worker_task`, reading `x->last_analysis_frame` as $5,292,000$.
3. Mid-chunk, a `clear` message arrives on the main thread. `mc_analyze_clear()` sets `x->current_sample_count = 0` under lock.
4. The worker thread finishes its chunk and updates `x->last_analysis_frame = 5,292,000 + 4,410 = 5,296,410`.
5. **The Result:** `x->last_analysis_frame` is left at **5,296,410**, while `x->current_sample_count` is reset to **0**!
6. DSP continues running. `current_sample_count` increments from 0: 4,410, 8,820, 13,230...
7. The condition `current_sample_count >= last_analysis_frame + hop_samples` remains **FALSE** for exactly **2 minutes** (until `current_sample_count` catches back up to 5,296,410)!

During those 2 minutes, no worker tasks are enqueued, no outlets emit data, and no visualizer packets are sent. The object appears completely dead. **Then, exactly 2 minutes later, `current_sample_count` crosses 5,296,410, and the object suddenly resumes normal operation on its own.** This explains the user's exact empirical observation.

---

### Cause B: Massive Catch-Up Backlog Loop on the Worker Thread
If the reverse counter mismatch occurs—where `x->last_analysis_frame` remains near `0` while `x->current_sample_count` continues growing—the worker thread enters its catch-up loop:
```c
while (x->current_sample_count >= x->last_analysis_frame + hop_samples) {
    // Process 100ms hop across all N channels
    x->last_analysis_frame = target_analysis_frame;
}
```
If `current_sample_count` is 5,000,000 and `last_analysis_frame` is 0, the worker loop must execute **1,133 sequential iterations across $N$ channels** ($1,133 \times N$ calls to `analyzer_analyze_chunk()`).

On a multi-channel setup, processing thousands of backlogged hops sequentially consumes 100% CPU of the background worker thread for **several minutes**. While stuck in this massive loop, real-time telemetry updates appear frozen or delayed until the worker completes the loop, logs `"catch-up complete: processed 1133 hops"`, and returns to real-time synchronization.

---

### Cause C: Unsynchronized Memory Freeing & Use-After-Free in Snapshot Lists
When `mc_analyze_clear()` is called on the main thread:
```c
critical_enter(x->lock);
for (long i = 0; i < x->analyzers_count; i++) {
    if (x->analyzers[i]) {
        analyzer_clear(x->analyzers[i]);
    }
}
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
`mc_analyze_worker_task` calls `analyzer_analyze_chunk()` **without holding `x->lock`** throughout the entire analysis loop (to avoid audio DSP lock contention). If `analyzer_clear()` runs on the main thread while the worker thread is traversing `self->snapshot_heads[b]`, a **Use-After-Free (heap corruption)** occurs, which can crash the worker thread or corrupt heap memory.

---

### Cause D: Permanent Lockup of `pending_analysis`
If an exception or crash occurs in the worker thread as a result of Cause C (Use-After-Free) or corrupted memory during the clear race, `x->pending_analysis` remains stuck at `1`.

Once `pending_analysis == 1`, `mc_analyze_perform64` will **never enqueue another worker task again**. Unlike Cause A (which recovers after a calculated sample delay), Cause D represents a permanent freeze that requires turning DSP off and on to clear.

---

## 3. Comparative Summary

| Metric / Behavior | `analyze~` (Single-Channel) | `mc.analyze~` (Multichannel) |
| :--- | :--- | :--- |
| **Worker Task Duration** | Very short (~1ms) | Long ($N \times 1\text{ms}$, e.g. 16–32ms) |
| **Collision Likelihood** | Low | Very High |
| **Time-Gap Mismatch Stall** | Minor (few milliseconds) | Major (minutes equal to track length prior to clear) |
| **Catch-Up Backlog Load** | Lightweight | Heavy (thousands of multi-channel STFT iterations) |
| **Consequence of Race** | Brief glitch or dropped frame | Severe: Minutes-long silent freeze, massive CPU backlog, or worker crash |

---

## 4. Proposed Architectural Solutions

To resolve these issues permanently in `mc.analyze~` (and `analyze~`), the following thread-synchronization safeguards are recommended:

1. **Worker Task Cancellation / Drain on Clear**:
   Before resetting state in `mc_analyze_clear()`, drain or cancel any in-flight worker task:
   ```c
   // Ensure background worker thread is idle before modifying structures
   async_worker_drain(x->worker);
   ```
2. **Atomic Counter Reset Under Lock**:
   Ensure `x->current_sample_count`, `x->last_analysis_frame`, and `x->audio_buffer_write_ptr` are updated atomically while holding `x->lock`, and that the worker thread reads/writes `last_analysis_frame` under `x->lock` to prevent time-gap mismatches.
3. **Worker Interlock / Generation Counter**:
   Add a monotonic `clear_sequence` counter to `t_mc_analyze`.
   - `mc_analyze_clear()` increments `x->clear_sequence`.
   - `mc_analyze_worker_task()` checks `clear_sequence` at the start of each hop. If the sequence counter changed mid-chunk, the worker immediately aborts processing the current hop and safely exits.
4. **Protect Snapshot Modifications in `cumulative_transience.c`**:
   Ensure `analyzer_clear()` and `analyzer_analyze_chunk()` synchronize snapshot list access using `self->lock_func` / `self->unlock_func` across all node traversals and node deletions.
5. **Sanitize `samples_ago` Calculations**:
   Clamp `samples_ago` to non-negative values (`if (samples_ago < 0) samples_ago = 0;`) to protect against negative pointer arithmetic during state resets.

---

## Conclusion

The intermittent, multi-minute freezing of `mc.analyze~` upon receiving a `clear` message is caused by sample-counter desynchronization between `x->current_sample_count` (reset to 0 by `clear`) and `x->last_analysis_frame` (retained at the pre-clear sample offset by a concurrent worker task). This creates a temporal gap equal to the elapsed song duration during which no analysis tasks are triggered, causing the object to freeze until real-time samples catch up. Combined with multi-channel worker task backlog loops and snapshot linked-list race conditions, implementing worker task draining and synchronized counter resets will eliminate this failure mode entirely.

# Technical Report: Speculated Causes and Proposed Solutions for Patch Freezes During `clear` Execution in `mc.analyze~`

## Executive Summary

This report investigates why sending a `clear` message during active audio playback causes Max patch freezes specifically in the multichannel `mc.analyze~` object, whereas the single-channel `analyze~` object does not exhibit this behavior.

**Key Operating Constraint**: This analysis assumes `@visualize 0` (the default state). With visualization disabled, zero CPU, memory, socket, or process overhead is spent on real-time visualization or TCP telemetry. Visualizers are explicitly ruled out as a contributing factor.

The issue stems entirely from structural differences between single-channel processing in `analyze~` and multichannel signal processing in `mc.analyze~`.

---

## Multichannel vs Single-Channel Architectural Differences

| Feature / Operation | Single-Channel `analyze~` | Multichannel `mc.analyze~` |
| :--- | :--- | :--- |
| **Worker Task Workload** | Processes 1 audio channel per hop | Loops across N audio channels sequentially per hop |
| **STFT & History Processing** | 1 STFT calculation & 1 history buffer per hop | N STFT calculations & N history buffers per hop |
| **Main-Thread Lock Hold Time** | Clears 1 analyzer & 1 buffer under `x->lock` | Clears N analyzers & N buffers under `x->lock` |
| **Memory Buffer Footprint** | ~2.64 MB audio buffer (15s @ 44.1kHz) | N x 2.64 MB audio buffers (~21.1 MB for 8 channels) |
| **Drain Wait Duration** | Microsecond wait in `async_worker_drain` | Millisecond-to-second wait in `async_worker_drain` |

---

## 1. Worker Thread Drain Delay in `async_worker_drain`

### Speculated Mechanism
When `mc_analyze_clear()` receives a `clear` message on Max's main thread, it calls:
```c
if (x->worker) {
    async_worker_drain(x->worker);
}
```
`async_worker_drain()` clears pending tasks from the worker queue and blocks the calling main thread via `systhread_cond_wait()` until the background worker thread finishes executing its current task (`worker->is_busy == 0`).

- **Why this affects `mc.analyze~` and NOT `analyze~`**:
  In `analyze~`, a single worker task processes only 1 channel of audio per hop, completing almost instantly.
  In `mc.analyze~`, a single worker task runs a `for (long ch = 0; ch < n_chans; ch++)` loop. For each channel, it performs full STFT chunk analysis, FFT calculations, peak scoring, and snapshot history updates. For 8, 16, or 32 channels, a single worker task takes **N times longer** to finish.

  When `clear` arrives, Max's main thread blocks in `async_worker_drain()` waiting for the heavy N-channel worker loop to finish. Under high DSP loads or large channel counts, this blocking wait causes Max's UI to freeze or hang.

### Proposed Solutions
1. **Asynchronous Non-Blocking Drain (Sequence Interlock)**:
   Instead of blocking the main thread in `async_worker_drain()`, increment a atomic `clear_sequence` counter on the object and clear the task queue without waiting for the running task to finish. Inside `mc_analyze_worker_task()`, check `x->clear_sequence` at the start of every channel loop iteration; if `clear_sequence` changes, abort the worker loop immediately.
2. **Per-Channel Worker Task Splitting**:
   Enqueue separate worker tasks per channel rather than a single monolithic task that iterates over all channels in a single blocking loop.

### Benefits & Downsides
- **Benefits**: Completely eliminates main-thread UI hangs during worker thread synchronization regardless of channel count.
- **Downsides**: Requires adding sequence check guards at every channel loop iteration in the background worker routines.

---

## 2. Multichannel Critical Section Lock Hold Time (`x->lock`)

### Speculated Mechanism
Inside `mc_analyze_clear()`, after draining the worker task, the main thread enters the object's critical section lock (`critical_enter(x->lock)`) to perform cleanup across all channels:
```c
critical_enter(x->lock);
x->clear_sequence++;
if (x->analyzers) {
    for (long i = 0; i < x->analyzers_count; i++) {
        if (x->analyzers[i]) {
            analyzer_clear(x->analyzers[i]);
        }
    }
}
if (x->audio_buffers) {
    for (long ch = 0; ch < x->allocated_audio_chans; ch++) {
        if (x->audio_buffers[ch]) {
            memset(x->audio_buffers[ch], 0, sizeof(float) * x->audio_buffer_size);
        }
    }
}
```
- **Why this affects `mc.analyze~` and NOT `analyze~`**:
  In `analyze~`, clearing involves 1 call to `analyzer_clear()` and 1 `memset()` on a 2.64 MB buffer.
  In `mc.analyze~`, clearing loops across `x->analyzers_count` (calling `analyzer_clear()` which deallocates/clears snapshot linked lists) and `x->allocated_audio_chans` (calling `memset()` across all channel audio buffers, totaling 21 MB to 84 MB of memory zeroing).

  Holding `x->lock` on the main thread for this duration prevents the 64-bit DSP audio perform routine (`mc_analyze_perform64`) from acquiring `x->lock`. When the DSP audio thread stalls waiting for `x->lock`, audio driver buffer underflows occur, causing Max's audio engine and UI to lock up.

### Proposed Solutions
1. **Atomic Pointer Swapping / Deferred Clearing**:
   Instead of clearing every channel's analyzer and zeroing memory synchronously inside `x->lock`, allocate clean empty buffers or swap pointers atomically under `x->lock`, and defer memory zeroing/deallocation to the background worker thread or outside the critical section.
2. **Per-Channel Critical Sections**:
   Replace the global `x->lock` with individual per-channel locks (`x->chan_locks[ch]`), allowing channel clearing to occur incrementally without blocking all channels or the DSP thread simultaneously.

### Benefits & Downsides
- **Benefits**: Keeps critical section lock hold times down to microseconds, preventing DSP thread starvation and audio driver freezes.
- **Downsides**: Increases code complexity surrounding memory pointer management and per-channel lock allocations.

---

## 3. Sample Counter and Buffer Index Race Conditions During Active Audio

### Speculated Mechanism
`mc_analyze_clear()` resets global sample counters:
```c
x->audio_buffer_write_ptr = 0;
x->current_sample_count = 0;
x->last_analysis_frame = 0;
x->pending_analysis = 0;
```
- **Why this affects `mc.analyze~` and NOT `analyze~`**:
  `mc.analyze~` processes audio frames across multiple channels concurrently. If audio continues playing on the DSP thread while `clear` resets `x->current_sample_count` to `0`, channel analysis loops in an active or queued worker task calculate buffer offsets (`samples_ago = cur_samples - hop_start_samples`).

  With N channels active, the likelihood of a worker task processing channel `K` using pre-clear frame indices while channel `K-1` has been zeroed is N times higher. Out-of-bounds array index calculations or clamped read pointer spins in the worker task can cause high CPU utilization and application freezes.

### Proposed Solutions
1. **Monotonic Generation Sequence Interlock**:
   Snapshot `start_seq = x->clear_sequence` before entering the multichannel processing loop in `mc_analyze_worker_task()`. If `x->clear_sequence != start_seq` at any point during channel iteration, discard intermediate results and exit immediately.
2. **Strict Bounds Sanitization**:
   Clamp all calculated `samples_ago` values strictly to `[0, audio_buffer_size - 1]` before indexing into `x->audio_buffers[ch]`.

### Benefits & Downsides
- **Benefits**: Guarantees thread-safe handling of sample counter resets during continuous multichannel audio playback.
- **Downsides**: Requires checking sequence tags before processing each channel in background worker tasks.

---

## Summary Conclusion

The reason Max patch freezes occur with `mc.analyze~` and not `analyze~` (when `@visualize 0` is set) is due to **multichannel scaling**:
1. **`async_worker_drain()`** blocks Max's main thread while the worker finishes a long, blocking loop across all N channels.
2. **`critical_enter(x->lock)`** holds the global object lock while clearing N analyzer instances and zeroing megabytes of multichannel audio memory, starving the audio DSP thread.

Eliminating the blocking wait in `async_worker_drain()` using an atomic sequence interlock and minimizing lock hold times inside `mc_analyze_clear()` will completely resolve these freezes.

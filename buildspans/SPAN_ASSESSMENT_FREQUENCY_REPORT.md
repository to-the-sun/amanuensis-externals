# Technical Report: Investigating Span Assessment Frequency in the `buildspans` Max External

## Executive Summary

The `buildspans` Max external is responsible for grouping incoming note events (which consist of absolute timestamps and velocity/resonance scores) into cohesive temporal segments called "spans," organized hierarchically by palette, track, and bar. A critical operation in this lifecycle is the **discontiguity and rating assessment**, which determines whether an active span should continue to grow or be finalized and ended.

This report investigates why the `buildspans` object may not be assessing whether a span should continue or end as frequently as it should (i.e., immediately after every completed bar). By analyzing the C source code (`buildspans.c`), we have identified several architectural, mathematical, and state-driven causes that limit the frequency and immediacy of these assessments.

---

## 1. Core Architectural Cause: Note-Driven (Event-Driven) vs. Clock-Driven Design

The most significant factor preventing `buildspans` from evaluating spans immediately after every completed bar is its **fundamentally event-driven (reactive) architecture**.

### How the Code Behaves
Discontiguity assessments are primarily evaluated within the `buildspans_process_and_add_note` function:

```c
// --- Find the most recent bar for the current track to see if this is a new bar ---
long last_bar_timestamp = -1;
...
int is_new_bar = (last_bar_timestamp == -1 || bar_timestamp_val > last_bar_timestamp);

// --- Deferred span ending logic (only if a new bar is detected) ---
if (is_new_bar && last_bar_timestamp != -1) {
    buildspans_check_discontiguity(x, x->current_palette, track_sym, relative_timestamp);
}
```

### The Problem of "The Silent Track"
Because this check is nested inside `buildspans_process_and_add_note`, it is **only triggered when a new note is received** on a track.
* **No Real-Time Timeout**: If a track becomes silent (e.g., the musician stops playing or the MIDI clip ends), **no new notes arrive**.
* **Zero Execution Paths**: Because no new notes are sent to the first inlet, `buildspans_process_and_add_note` is never executed for that track, meaning `buildspans_check_discontiguity` is never called.
* **The Span Hangs**: The active span remains open and unassessed indefinitely in the `building` dictionary. It will only be forced closed much later by a global `bang`, `flush`, or `clear` message, or if a note is finally received after a long period of silence.

### The Contrast with a Clock-Driven Model
To evaluate a span precisely at the boundary of a completed bar, the object would require an internal scheduler, timer, or polling clock (such as a Max `t_clock` or signal-rate transition detector) that periodically checks active tracks and evaluates their state *independent of incoming note events*. Currently, no such active polling of the dictionary exists for real-time note stream silence.

---

## 2. Dependency on External Offset Updates (`buildspans_do_offset`)

While `buildspans` does not have an internal real-time clock to check for silent tracks, it does provide a secondary entry point for discontiguity assessment in `buildspans_do_offset` (which is triggered when an `offset` message is received on the second inlet).

```c
// --- MODIFIED DISCONTIGUITY CHECK PHASE ---
// Conduct a modified discontiguity check for every track across every palette BEFORE duplication.
long num_keys_check;
t_symbol **keys_check;
dictionary_getkeys(x->building, &num_keys_check, &keys_check);
if (keys_check) {
    ...
    buildspans_check_discontiguity(x, gensym(pal_str), gensym(track_str), relative_f);
    ...
}
```

### The Frequency Vulnerability
This multi-track assessment is entirely dependent on the **frequency and timing of external offset messages**.
* If the host Max patcher only sends an `offset` update at the start of a song, or sparsely during section transitions, `buildspans` will not check active spans for discontiguity during steady-state playback.
* If the offset playhead updates are jittery, delayed, or throttled to save CPU, the assessment window will be pushed past the completed bar boundary, causing late or missed closures.

---

## 3. Mathematical Thresholds of the Discontiguity Math

Even when a discontiguity assessment is triggered (either via a new note or an offset update), the mathematical logic inside `buildspans_check_discontiguity` prevents a span from ending immediately after a single completed bar of silence.

### The Formula
Let's examine the comparison math:
```c
double gap_limit = (double)most_recent_bar_after_rating_check + 2.0 * (double)bar_length;
int is_discontiguous = (relative_comparison_val > gap_limit);
```

### The 2-Bar Tolerance Gap
* **The Threshold**: A span is only declared discontiguous if the current relative time (`relative_comparison_val`) is **strictly greater than two full bar lengths** past the start of the most recently recorded bar.
* **Why This Prevents 1-Bar Closures**: This formula is designed to allow a "one-bar rest" (allowing musical pauses without immediately shattering the span). However, this means a span *cannot* mathematically end after exactly one completed bar of silence. It must wait until the time elapsed exceeds **two full bar lengths**.
* **Delayed Action**: Consequently, if a musician plays a phrase and stops, the span is not assessed as completed until more than 2.0 bars of silence have elapsed, which creates a noticeable latency in span ending and output.

---

## 4. Latency and Offloading in Async/Deferred Modes

The `buildspans` object supports asynchronous (`@async 1`) and deferred (`@defer 1`) execution to prevent blocking the high-priority Max scheduler or audio threads (especially during expensive dictionary manipulations).

### The Worker Queue Delay
When `@async` is enabled, messages are enqueued to a background worker:
```c
if (x->async && x->worker && !async_worker_is_worker_thread(x->worker)) {
    async_worker_enqueue(x->worker, x, (method)buildspans_offset_deferred, NULL, 1, &a);
    return;
}
```
* **Thread Context Switching**: The actual processing of notes, offsets, and discontiguity checks is shifted to a background worker thread.
* **Queue Congestion**: If the worker thread is busy duplicating other spans (which is an $O(N)$ or $O(N^2)$ dictionary operation depending on the migration phase) or executing heavy rating checks, the discontiguity checks for active tracks are queued.
* **Missed Boundaries**: By the time the worker thread dequeues and runs the check, the current musical time may have advanced significantly, causing assessments to occur multiple beats or bars after they were musically "due."

---

## 5. Buffer-Bound Bar Length Latency and Desynchronization

The duration of a bar (`bar_length`) is crucial for calculating bar timestamps and checking gaps. The object retrieves this value via `buildspans_get_bar_length`:

```c
long buildspans_get_bar_length(t_buildspans *x) {
    if (x->local_bar_length > 0) {
        return (long)x->local_bar_length;
    }
    ...
    t_buffer_obj *b = buffer_ref_getobject(x->buffer_ref);
    ...
    float *samples = buffer_locksamples(b);
    if (samples) {
        if (buffer_getframecount(b) > 0) {
            bar_length = (long)samples[0];
        }
        ...
    }
    ...
}
```

### The Desynchronization Risk
* **Dynamic Tempo Changes**: If the tempo changes dynamically, the host patcher must update the value in the `bar` buffer.
* **Stale Caching**: Once retrieved, `local_bar_length` is cached to avoid locking the buffer on every single note. If the tempo changes but `local_bar_length` is not cleared or re-read, the object will use an outdated bar length.
* **Incorrect Bar Boundaries**: An incorrect bar length will cause the calculated bar timestamps (`bar_timestamp_val = floor(relative_timestamp / bar_length) * bar_length`) to drift from the actual grid. This misaligns the `is_new_bar` detection and causes the discontiguity check to fire at arbitrary musical times instead of exact bar boundaries.

---

## Summary of Potential Solutions and Remedies

To ensure that `buildspans` assesses span continuation/ending precisely after every completed bar, several structural adjustments can be considered:

1. **Implement an Active Timer (Time-Driven Evaluation)**:
   Add a periodic `t_clock` (e.g., ticking every 100-200ms or synchronized to the transport) that iterates over active spans in the registry and evaluates discontiguity without waiting for incoming note events. This would resolve the "silent track" issue completely.

2. **Adjust the Gap Threshold Constraint**:
   If a more aggressive 1-bar boundary is desired, the gap tolerance multiplier in `buildspans_check_discontiguity` can be adjusted from `2.0` to a value closer to `1.0` (e.g., `1.0 * bar_length` or `1.5 * bar_length`), allowing spans to end sooner.

3. **Synchronize Offset Clock with Bar Boundaries**:
   Ensure that the external Max patcher sends high-priority `offset` messages to the second inlet exactly at every bar boundary, ensuring a clean and consistent assessment pass across all active tracks.

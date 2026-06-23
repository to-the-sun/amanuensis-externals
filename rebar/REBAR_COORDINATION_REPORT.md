# Internal Coordination and Communication in the `rebar` Object

This report details the communication and coordination mechanisms between the internal modules (`notify`, `buildspans`, and `crucible`) of the `rebar` composite object, specifically focusing on the differences between synchronous (`@async 0`) and asynchronous (`@async 1`) execution.

## 1. Structural Overview

The `rebar` object is a composite object that encapsulates three internal modules. It uses a "source-level inclusion" strategy where the C source files of the modules are included directly into `rebar.c` after certain symbols are renamed via macros to avoid linker collisions.

### Module Responsibilities:
- **`notify`**: Scans the provided dictionary and "dumps" note data (absolute time, score, track, palette).
- **`buildspans`**: Receives individual notes and groups them into "spans" based on chronological bars and track IDs.
- **`crucible`**: Receives completed spans, compares them against an "incumbent" dictionary, and manages "reaches" (how much of a song has been high-quality rendered).

## 2. Coordination Mechanisms

`rebar` employs three primary methods to coordinate these modules:

### A. Outlet Interception (The "Virtual Wire")
`rebar` redefines the standard Max SDK outlet functions (`outlet_new`, `outlet_anything`, `outlet_int`, `outlet_float`, `outlet_bang`) for the internal modules. When a module "outputs" data, it is actually calling a `rebar` interceptor.

- **Routing Table (Simplified):**
    - `notify` -> `buildspans`: Notes, Tracks, and Offsets are intercepted and passed directly to the corresponding `buildspans` C functions (e.g., `buildspans_list`).
    - `buildspans` -> `crucible`: Completed spans are intercepted and passed to `crucible_anything`.
    - `crucible` -> `rebar` Outlets: Reach and status data are intercepted and passed to the real `rebar` outlets.
    - All modules -> `rebar` Log: Log messages are consolidated into a single `rebar` log outlet.

### B. Direct Binding
In addition to outlet interception, `rebar` explicitly binds `buildspans` to `crucible` at instantiation:
```c
x->buildspans_inst->bound_crucible = (void *)x->crucible_inst;
object_attach_byptr(x->buildspans_inst, x->crucible_inst);
```
This allows `buildspans` to call `crucible_anything` directly, bypassing the intercepted outlet system for certain core operations.

### C. Shared Asynchronous Worker
When `@async 1` is enabled, `rebar` creates a single `t_async_worker` (background thread) and shares it among all three internal modules. This ensures that all heavy dictionary processing happens on a single background thread, maintaining thread safety for shared dictionary access.

---

## 3. Synchronous Coordination Flow (`@async 0`)

When a trigger (e.g., an `int` message) hits `rebar`:

1.  **Main Thread:** `rebar` receives the message and calls `notify_bang`.
2.  **Main Thread:** `notify` iterates the dictionary and outputs notes via `outlet_list`.
3.  **Main Thread:** `rebar` intercepts the `outlet_list` call and immediately calls `buildspans_list`.
4.  **Main Thread:** `buildspans` processes the note. If a span is completed, it calls `crucible_anything` (either via intercepted outlet or direct binding).
5.  **Main Thread:** `crucible` performs the comparison and updates the incumbent dictionary. It then outputs reach data via `outlet_anything`.
6.  **Main Thread:** `rebar` intercepts this and sends it to the real external outlet.

In this mode, the entire pipeline is linear and occurs entirely on the Main Thread.

---

## 4. Asynchronous Coordination Flow (`@async 1`)

The asynchronous mode introduces a "ping-pong" effect between the background worker thread and the main thread due to the interplay of `async_worker_enqueue` and `defer`.

### The "Ping-Pong" Execution Trace:

1.  **Main Thread:** `rebar` receives a trigger and **enqueues** the task to the shared worker.
2.  **Worker Thread:** The worker picks up the task and starts `notify_do_bang`.
3.  **Worker Thread:** `notify` processes a note. Because it is in "async" mode and *not* on the main thread, it **defers** its output:
    `defer(x, (method)notify_defer_output, gensym("abs_score"), ...)`
4.  **Main Thread:** The deferred `notify_defer_output` fires. It calls `outlet_list`.
5.  **Main Thread:** `rebar` intercepts the `outlet_list` call and calls `buildspans_list`.
6.  **Main Thread:** `buildspans_list` sees that it is on the Main Thread but `async` is enabled. To stay off the main thread for processing, it **enqueues** the note BACK to the worker:
    `async_worker_enqueue(x->worker, x, (method)buildspans_do_list, ...)`
7.  **Worker Thread:** The worker picks up the note and starts `buildspans_do_list`.
8.  **Worker Thread:** `buildspans` processes the note. If a span completes, it calls `crucible_anything` directly.
9.  **Worker Thread:** `crucible_do_anything` processes the span. Because it is in "async" mode and *not* on the main thread, it **defers** its reach output:
    `defer(x, (method)crucible_defer_output, ...)`
10. **Main Thread:** The deferred `crucible_defer_output` fires and finally sends the data to the real Max outlets.

### Why the Ping-Pong?
This happens because `notify` was originally designed as a standalone object that only communicates via outlets. `rebar` leverages these outlets to "wire" it to `buildspans`. However, since `buildspans` also implements its own async logic (enqueuing anything it receives on the main thread), the data must return to the main thread briefly to be intercepted before being sent back to the worker for the next stage of processing.

## 5. Summary Table

| Feature | Synchronous (`@async 0`) | Asynchronous (`@async 1`) |
| :--- | :--- | :--- |
| **Execution Thread** | 100% Main Thread | Hybrid (Worker <-> Main) |
| **`notify` -> `buildspans`** | Direct C Call (Intercepted) | Deferred -> Main -> Enqueued -> Worker |
| **`buildspans` -> `crucible`** | Direct C Call | Direct C Call (Same Worker Thread) |
| **Data Output** | Immediate | Deferred to Main Thread |
| **Dictionary Safety** | Single-threaded | Protected by shared worker queue |

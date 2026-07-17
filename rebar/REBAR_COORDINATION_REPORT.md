# Internal Coordination and Communication in the `rebar` Object

> **Deprecation Notice:** A native `rebar` function has been directly integrated into the `crucible` object (via a `rebar` message). The `rebar` composite object described below is now obsolete and superseded by the direct `crucible` integration.

This report details the communication and coordination mechanisms between the internal modules (`notify`, `buildspans`, and `crucible`) of the `rebar` composite object, specifically focusing on the differences between synchronous (`@async 0`) and asynchronous (`@async 1`) execution.

## 1. Structural Overview

The `rebar` object is a composite object that encapsulates three internal modules. It uses a "source-level inclusion" strategy where the C source files of the modules are included directly into `rebar.c` after certain symbols are renamed via macros to avoid linker collisions.

### Module Responsibilities:
- **`notify`**: Scans the provided dictionary and "dumps" note data (absolute time, score, track, palette).
- **`buildspans`**: Receives individual notes and groups them into "spans" based on chronological bars and track IDs.
- **`crucible`**: Receives completed spans, compares them against an "incumbent" dictionary, and manages "reaches" (how much of a song has been high-quality rendered).

## 2. Coordination Mechanisms

`rebar` employs three primary methods to coordinate these modules:

### A. Direct Binding (Full Pipeline)
All internal modules of the `rebar` object are bound to each other by default. This is the primary and most efficient communication method.
- **`notify` -> `buildspans`**: `notify` holds a pointer to the `buildspans` instance and calls its `do_` functions (e.g., `buildspans_do_list`) directly.
- **`buildspans` -> `crucible`**: `buildspans` holds a pointer to the `crucible` instance and calls its `do_` functions (e.g., `crucible_do_anything`) directly.

This binding exists whether or not `@async` is enabled, ensuring that data flows through the entire processing pipeline without ever leaving the current thread (Main Thread in sync mode, Worker Thread in async mode).

### B. Outlet Interception (Legacy/Fallback)
`rebar` redefines the standard Max SDK outlet functions for the internal modules. While direct binding is now the primary path, the interception system remains to handle any messages not covered by direct calls and to consolidate logging into the `rebar` log outlet.

### C. Shared Asynchronous Worker
When `@async 1` is enabled, `rebar` creates a single `t_async_worker` (background thread) and shares it among all three internal modules. This ensures that all heavy dictionary processing happens on a single background thread, maintaining thread safety for shared dictionary access.

---

## 3. Synchronous Coordination Flow (`@async 0`)

When a trigger (e.g., an `int` message) hits `rebar`:

1.  **Main Thread:** `rebar` receives the message and calls `notify_bang`.
2.  **Main Thread:** `notify` iterates the dictionary and calls `buildspans_do_list` directly for each note.
3.  **Main Thread:** `buildspans` processes the notes. Upon span completion, it calls `crucible_do_anything` directly.
4.  **Main Thread:** `crucible` performs the comparison and updates the incumbent dictionary. It then **defers** its final reach output to the end of the event.
5.  **Main Thread:** The final outputs are sent to the real external outlets.

The entire pipeline is linear and occurs entirely on the Main Thread.

---

## 4. Asynchronous Coordination Flow (`@async 1`)

With full direct binding, the "ping-pong" effect has been eliminated. The data stays on the background worker thread until the very end of the pipeline.

### The Optimized Execution Trace:

1.  **Main Thread:** `rebar` receives a trigger and **enqueues** the task to the shared worker.
2.  **Worker Thread:** The worker picks up the task and starts `notify_do_bang`.
3.  **Worker Thread:** `notify` processes notes and calls `buildspans_do_list` **directly** on the same worker thread.
4.  **Worker Thread:** `buildspans` processes the notes. Upon span completion, it calls `crucible_do_anything` **directly** on the same worker thread.
5.  **Worker Thread:** `crucible` performs comparisons and dictionary updates.
6.  **Worker Thread:** Once the entire pipeline is complete, the final status update and dictionary synchronization are triggered. Any external outlet output (reaches, status) is **deferred** back to the Main Thread.
7.  **Main Thread:** The deferred outputs fire and send data to the real Max outlets.

### Efficiency Gains:
By binding the modules and using direct C calls, we avoid the overhead of multiple `defer` and `enqueue` cycles. The data moves from `notify` to `crucible` at C-function-call speed, entirely within the background worker thread, maximizing throughput and minimizing Main Thread jitter.

## 5. Summary Table

| Feature | Synchronous (`@async 0`) | Asynchronous (`@async 1`) |
| :--- | :--- | :--- |
| **Execution Thread** | 100% Main Thread | Hybrid (Trigger on Main, Process on Worker) |
| **`notify` -> `buildspans`** | Direct C Call | Direct C Call (Same Worker Thread) |
| **`buildspans` -> `crucible`** | Direct C Call | Direct C Call (Same Worker Thread) |
| **Main Thread Interactions** | During Trigger & Output | During Trigger & Final Output ONLY |
| **Data Output** | Immediate | Deferred to Main Thread |
| **Dictionary Safety** | Single-threaded | Protected by shared worker queue |

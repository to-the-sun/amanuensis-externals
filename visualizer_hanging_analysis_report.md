# Visualizer Freezing and Resource Spike Analysis Report

## Executive Summary

Following a song export in Max, the patch readies itself for a new song by executing scripting commands that bulk-delete and recreate multiple `analyze~` and `mc.analyze~` objects. During this transition, Max experiences severe hanging/freezing, accompanied by 100% CPU utilization and/or 100% RAM utilization in Windows Task Manager.

This report provides a technical root cause analysis based on the codebase architecture (`shared/visualize.c`, `analyze~.c`, `mc.analyze~.c`, `transience_vis.py`, and `visualizer.py`). The investigation reveals that the recently implemented automatic visualizer closing and dynamic port lifecycle logic creates synchronous blocking on Max's main thread, process creation storms, socket port collisions, and orphaned background Python processes.

---

## Architectural Overview

### 1. Object Lifecycle and Visualizer Spawning
Each instance of `analyze~` (or each audio channel in `mc.analyze~`) allocates a dynamic TCP port starting at 9001 via `visualize_allocate_port()`. Upon initialization or attribute configuration, the object calls `launch_visualizer()` (or `launch_visualizers()`), executing `CreateProcessA` to spawn a background Python process (`python transience_vis.py --port <PORT> ...`).

### 2. Telemetry and Communication Queue
Telemetry messages are routed through `visualize_to_port()`, which enqueues JSON packets into a per-port worker thread structure (`t_dynamic_socket`). A dedicated C worker thread (`dynamic_socket_worker_thread`) drains this queue and sends data over non-blocking TCP sockets (`perform_send()`).

### 3. Destruction and Automatic Shutdown Sequence
When `analyze~` or `mc.analyze~` objects are freed (`analyze_free` / `mc_analyze_free`):
1. A JSON packet `{"type":"analyze","event":"close",...}` is enqueued via `visualize_to_port()`.
2. The object immediately calls `visualize_close_port(x->viz_port)`.
3. `visualize_close_port()` invokes `free_dynamic_socket_queue()`, which sets `ds->exit_flag = 1`, signals the worker condition variable, and calls `systhread_join(ds->thread, &ret)`.
4. Finally, `visualize_cleanup()` decrements the global reference count `ref_count`.

---

## Detailed Investigation and Root Cause Analysis

### Cause 1: Synchronous Thread Join Blocking on Max Main Thread
In `analyze_free` and `mc_analyze_free`, the `"close"` packet is enqueued, and `visualize_close_port()` is called in the very next line of C code.
Inside `visualize_close_port()`, `free_dynamic_socket_queue()` calls `systhread_join(ds->thread, &ret)` on the thread executing object destruction (Max's main UI/audio thread).

While waiting to join:
* The worker thread (`dynamic_socket_worker_thread`) attempts to execute `perform_send()`.
* If the companion Python process has already disconnected or is in the middle of shutting down, `perform_send()` calls `ensure_connected()`, which attempts a non-blocking `connect()` followed by a `select()` call with a 50ms to 1000ms timeout.
* If 20 to 50 `analyze~` objects are deleted sequentially during scripting patcher reloads, Max's main thread blocks for 50ms to 1000ms per object. 50 objects x 500ms = 25 seconds of complete application freezing.

### Cause 2: Process Storm and Resource Spikes (100% CPU and 100% RAM)
When scripting commands destroy and immediately recreate 20 to 50 `analyze~` / `mc.analyze~` objects:
* The deletion enqueues closing signals, while the creation immediately calls `CreateProcessA` for 20 to 50 new Python processes.
* Each new Python process imports heavy dependencies (`pyray`, Raylib C-extensions, NumPy, Pygame) and initializes OpenGL/GLFW window contexts.
* **CPU Spike (100%)**: Spawning 20 to 50 Python interpreters simultaneously pins all CPU cores to maximum capacity during module imports and window creation.
* **RAM Spike (100%)**: Each standalone Python process with Raylib/Pygame runtime consumes between 60 MB and 150 MB of system memory. 30 to 50 active or lingering Python processes rapidly accumulate 3 GB to 8 GB+ of RAM, exhausting physical memory and triggering heavy disk swapping.

### Cause 3: TCP Port TIME_WAIT Collisions and Orphaned Processes
When a TCP socket closes on Windows, the operating system holds the socket port in a `TIME_WAIT` state for 30 to 60 seconds.
* `visualize_allocate_port()` checks port availability by binding a temporary C socket.
* If a port was recently closed, C test binding may succeed, but when the newly spawned `transience_vis.py` process attempts `server_sock.bind(("", TCP_PORT))`, Windows rejects it with `WSAEADDRINUSE`.
* The Python process prints a bind error and exits immediately (`sys.exit(1)`).
* However, the C object in Max assumes the visualizer on that port is active. Consequently, `visualize_to_port()` continues queuing telemetry, causing `perform_send()` to repeatedly fail and time out on `select()`, introducing persistent background lag.
* Conversely, if old Python processes fail to receive the `"close"` packet because `free_dynamic_socket_queue()` purged the queue or closed the socket prematurely, old Python processes remain running as orphaned background processes, holding onto system memory and CPU cycles.

### Cause 4: Global Library Tear-Down and Re-Initialization Flapping
When Max scripting deletes all existing `analyze~` objects before creating new ones, `ref_count` in `shared/visualize.c` drops to 0.
* `visualize_cleanup()` triggers global library shutdown: joining `viz_thread`, closing static sockets (`crucible_viz`, `weaver_viz`), calling `WSACleanup()`, and unmapping shared memory (`UnmapViewOfFile`).
* A millisecond later, scripting creates new `analyze~` objects, calling `visualize_init()` to re-initialize Winsock, re-create mutexes, and re-map shared memory.
* Tearing down and re-building global networking and shared memory state mid-flight creates severe kernel lock contention and thread synchronization hazards.

---

## Proposed Solutions

### Proposal 1: Asynchronous Non-Blocking Queue Shutdown (C Engine)

#### Description
Modify `visualize_close_port()` and `free_dynamic_socket_queue()` so that `systhread_join` is not called synchronously on Max's main thread during object destruction. Instead:
1. Mark `exit_flag = 1` on the dynamic socket structure.
2. Signal `queue_cond`.
3. Detach or schedule the worker thread to exit cleanly in the background, or set a zero-timeout non-blocking socket drain, allowing `analyze_free` to return immediately without waiting for TCP socket timeouts.

#### Potential Benefits
* Completely eliminates Max main-thread freezing during scripting object reloads.
* Scripting patcher updates execute instantaneously.

#### Potential Downsides
* Requires thread-safe memory management so that dynamic socket structures are freed safely after the background worker thread has completely exited.

---

### Proposal 2: Windows Job Objects Process Binding (OS Kernel Safeguard)

#### Description
In `analyze~.c` and `mc.analyze~.c`, create a Windows Job Object (`CreateJobObjectA`) with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` configured. Assign all spawned child Python processes to this Job Object via `AssignProcessToJobObject`.

#### Potential Benefits
* 100% guaranteed clean termination of all spawned Python visualizer processes whenever Max closes or the job handle is released.
* Prevents orphaned Python processes under all circumstances (including process crashes or network dropouts).
* Instant OS-level memory reclamation without relying on TCP message delivery.

#### Potential Downsides
* Windows-specific C API (though fully compatible with the 64-bit Windows `.mxe64` target environment).

---

### Proposal 3: Visualizer Process Pooling and Re-use

#### Description
Instead of destroying and spawning new Python processes every time scripting recreates `analyze~` objects, maintain a persistent pool of visualizer windows or re-bind existing running visualizer instances to new object IDs and ports.

#### Potential Benefits
* Completely prevents CPU and RAM spikes caused by repeated Python process initialization.
* Eliminates TCP port `TIME_WAIT` conflicts.

#### Potential Downsides
* Requires significant refactoring of process management logic in both C and Python.

---

### Proposal 4: Persistent Reference Count and Re-bind Safeguard in `shared/visualize.c`

#### Description
Prevent `visualize_cleanup()` from executing full Winsock `WSACleanup()` and shared memory unmapping immediately when `ref_count` drops to 0 if an object re-creation is imminent (e.g., deferring cleanup by a short grace period or maintaining persistent library state while Max is running).

#### Potential Benefits
* Prevents Winsock and shared memory corruption during scripting patcher reloads.
* Stabilizes global thread state across song transitions.

#### Potential Downsides
* Keeps Winsock initialized until Max exits.

---

### Proposal 5: Robust Socket Bind Retries and Heartbeat in Python Visualizers

#### Description
In `transience_vis.py`:
1. Add a retry loop for `server_sock.bind()` (e.g. 10 attempts with 100ms sleeps) to handle socket `TIME_WAIT` transitions gracefully.
2. Add a parent process check (e.g. monitoring parent PID health) or socket heartbeat so visualizers automatically terminate if their parent C object disappears without sending a `"close"` packet.

#### Potential Benefits
* Eliminates visualizer launch failures due to transient socket `TIME_WAIT` states.
* Ensures orphaned visualizers automatically self-terminate.

#### Potential Downsides
* Python-side change only; must be combined with Proposal 1 to eliminate Max main-thread hanging.

---

## Conclusion and Recommended Next Steps

The freezing and resource spikes observed after song exports stem from a combination of synchronous thread joins in C during object destruction, process creation storms during scripting reloads, and TCP socket port collisions.

### Recommended Action Plan:
1. **Implement Proposal 1 (Asynchronous Queue Shutdown)**: Eliminate synchronous `systhread_join` blocking on Max's main thread inside `shared/visualize.c`.
2. **Implement Proposal 2 (Windows Job Objects)**: Bind spawned visualizer processes to a job object for instant, guaranteed process cleanup.
3. **Implement Proposal 5 (Bind Retry & Heartbeat)**: Add robust socket bind retries and parent PID monitoring in `transience_vis.py`.

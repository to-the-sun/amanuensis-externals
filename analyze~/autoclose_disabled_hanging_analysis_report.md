# Visualizer Hanging Analysis Report: Disabling Auto-Close Mechanics

## Executive Summary

To test whether companion visualizer shutdown logic was responsible for Max patch freezing during song exports and scripting patcher reloads, the auto-closing mechanisms in `transience_vis.py` were disabled. Specifically, the 15-second initial connection timeout, client disconnect exit flags, and `'close'` event handling were removed so that Python visualizer processes remain running independently.

However, Max continues to experience severe application hangs and freezes when destroying and recreating `analyze~` and `mc.analyze~` objects.

This report provides a deeper technical investigation into why disabling Python-side visualizer auto-close fails to resolve the freezing. It identifies five underlying root causes in the C external architecture (`shared/visualize.c`, `analyze~.c`, `mc.analyze~.c`), Winsock socket handling, multi-thread queue synchronization, and IPC lock contention that operate independently of Python visualizer process termination.

---

## Detailed Technical Investigation

### 1. Synchronous C Thread Joins on Max's Main UI/Audio Thread
When Max scripting bulk-deletes `analyze~` or `mc.analyze~` instances prior to creating new ones, `analyze_free()` or `mc_analyze_free()` calls `stop_visualizer()` / `stop_visualizers()`. In `analyze~.c`:

```c
static void stop_visualizer(t_analyze *x) {
    if (x->viz_port > 0) {
        // Enqueues unbind packet
        visualize_to_port(x, x->viz_port, "analyze", json_buf);
        visualize_release_port(x->viz_port);
        x->viz_port = 0;
    }
    if (x->viz_initialized) {
        visualize_cleanup();
        x->viz_initialized = 0;
    }
}
```

Even though Python processes no longer exit on disconnect, `stop_visualizer()` invokes `visualize_cleanup()`, which decrements the global `ref_count` in `shared/visualize.c`. When the last object instance is destroyed, `ref_count` reaches 0, triggering full library cleanup:

```c
void visualize_cleanup() {
    viz_lock_enter();
    ref_count--;
    if (ref_count <= 0) {
        if (queue_mutex) {
            systhread_mutex_lock(queue_mutex);
            viz_exit_flag = 1;
            systhread_cond_signal(viz_cond);
            systhread_mutex_unlock(queue_mutex);

            if (viz_thread) {
                unsigned int ret;
                systhread_join(viz_thread, &ret); // Synchronous block on Max UI Thread
                viz_thread = NULL;
            }
            ...
        }
    }
}
```

#### Why Freezing Persists
1. **Main-Thread Blocking**: `systhread_join(viz_thread, &ret)` is executed synchronously on Max's main application thread during object destruction.
2. **Socket Timeout Delay**: The background thread `viz_worker_thread` or per-port worker threads (`dynamic_socket_worker_thread`) may be in the middle of executing `perform_send()`.
3. **Select() Timeout Stalls**: Inside `perform_send()`, if a socket send operation encounters a blocked TCP window or socket reconnection, `ensure_connected()` executes `select()` with blocking timeouts (up to 1000ms).
4. **Cascade Effect**: Joining worker threads that are stuck inside Winsock `select()` or `send()` calls forces Max's main thread to wait for every worker thread to finish its active network timeout. Sequentially deleting 20 to 50 objects results in cumulative delays of tens of seconds.

---

## 2. Winsock Socket Teardown Flapping and Memory Re-allocation

When scripting deletes all existing `analyze~` objects and recreates new ones within milliseconds:

1. `ref_count` drops to 0, causing `visualize_cleanup()` to call `WSACleanup()`, free dynamic socket mutexes, and unmap shared memory view `UnmapViewOfFile(g_pSharedPortMap)`.
2. A millisecond later, newly instantiated `analyze~` objects call `visualize_init()`, executing `WSAStartup()`, re-creating condition variables and mutexes, and re-mapping shared memory via `CreateFileMappingA`.

#### Why Freezing Persists
Repeatedly tearing down and rebuilding global networking sub-systems and Windows shared memory mappings in rapid succession (`WSACleanup()` followed immediately by `WSAStartup()`) causes kernel lock contention in the Windows TCP/IP stack (`ws2_32.dll`). This introduces thread synchronization hazards and kernel-level stalls that lock up the calling process regardless of Python process state.

---

## 3. TCP Kernel Socket Buffer Backlog and Non-Blocking Winsock Timeouts

In `shared/visualize.c`, telemetry updates are sent over TCP localhost sockets using Winsock in non-blocking mode:

```c
static int perform_send(t_viz_socket *vs, void *x, const char *type, const char *message) {
    ...
    while (total_sent < len) {
        int sent = send(vs->sock, buf + total_sent, len - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
                fd_set write_fds;
                struct timeval tv = {1, 0}; // 1.0 second timeout!
                ...
                int sel_ret = select((int)vs->sock + 1, NULL, &write_fds, NULL, &tv);
                ...
            }
        }
    }
}
```

#### Why Freezing Persists
1. **High-Frequency Telemetry Volume**: Each `analyze~` instance queues 130KB JSON packets containing 4-band spectral envelopes, dynamic smoothings, and 5001-point accumulated transience buffers at 100ms intervals.
2. **Buffer Saturation**: When multiple instances stream large JSON payloads simultaneously, TCP socket send buffers fill up rapidly.
3. **1-Second Blocking Traps**: When `send()` returns `WSAEWOULDBLOCK`, `perform_send()` enters a `select()` retry loop with a 1-second timeout (`tv.tv_sec = 1`).
4. **Worker Thread Queue Bottleneck**: Because each dynamic socket worker thread handles pending items sequentially, worker threads spend seconds waiting for socket send buffers to drain. When object destruction triggers `free_dynamic_socket_queue()`, calling `systhread_join()` forces Max's UI thread to wait for these 1-second `select()` loops to complete.

---

## 4. Windows Named Mutex Lock Contention Across DLL Boundaries

Port allocation across `analyze~` and `mc.analyze~` instances relies on Windows Named Shared Memory (`Local\MaxAnalyzeVizSharedPorts`) protected by a Named Mutex (`Local\MaxAnalyzeVizPortMutex`):

```c
static void shared_port_map_lock(void) {
#if defined(WIN_VERSION) || defined(_WIN32)
    if (g_hSharedMutex) {
        WaitForSingleObject(g_hSharedMutex, INFINITE);
    }
#endif
}
```

#### Why Freezing Persists
1. **Infinite Wait Traps**: `WaitForSingleObject(g_hSharedMutex, INFINITE)` blocks indefinitely waiting for the system-wide named mutex.
2. **Cross-DLL Deadlocks**: When Max scripting recreates dozens of objects, separate threads across `analyze~.mxe64` and `mc.analyze~.mxe64` simultaneously attempt to allocate and release ports.
3. **Abandoned Mutex Hazards**: If a thread is interrupted or terminated while holding `g_hSharedMutex` during object destruction, subsequent calls to `shared_port_map_lock()` hang indefinitely on `WaitForSingleObject`, freezing Max completely.

---

## 5. Background Task Queue Contention in `shared/async_worker.c`

Each `analyze~` instance uses an `t_async_worker` background thread to perform transient DSP analysis chunks (`analyze_worker_task`). During object destruction or clearing, `analyze_clear()` calls `async_worker_drain(x->worker)`.

#### Why Freezing Persists
Inside `async_worker_drain()`:

```c
void async_worker_drain(t_async_worker* worker) {
    if (!worker) return;
    systhread_mutex_lock(worker->mutex);
    while (linklist_getsize(worker->queue) > 0 || worker->busy) {
        systhread_cond_wait(worker->cond, worker->mutex);
    }
    systhread_mutex_unlock(worker->mutex);
}
```

If background worker tasks are performing expensive STFT analysis or cumulative transience matrix updates when scripting deletes objects, `async_worker_drain()` forces Max's main thread to wait until all pending DSP tasks complete. When 20 to 50 objects are cleared and freed at once, this causes significant UI thread hesitation.

---

## Summary of Findings

Disabling visualizer auto-close in Python (`transience_vis.py`) leaves the GUI processes running, but **does not address the C-level blocking architecture inside Max**. The primary causes of Max hanging during song exports and scripting patcher reloads are:

1. **Synchronous `systhread_join()` Blocking**: Max's main thread waits for socket worker threads to exit during `visualize_cleanup()` and `visualize_close_port()`.
2. **1-Second `select()` Timeouts**: Socket worker threads stall for up to 1000ms inside `perform_send()` when TCP socket buffers fill up.
3. **Global Winsock Flapping**: Rapidly calling `WSACleanup()` and `WSAStartup()` during scripting reloads causes kernel-level socket lock contention.
4. **Infinite Named Mutex Waits**: `WaitForSingleObject(g_hSharedMutex, INFINITE)` creates potential deadlocks during multi-object port allocation.

---

## Actionable Architectural Remedies

To permanently eliminate Max freezing during scripting reloads without relying on Python auto-closing behavior:

1. **Non-Blocking Socket Closing in C**:
   In `visualize_close_port()`, immediately call `closesocket(ds->vs.sock)` *before* signaling `ds->queue_cond`. Closing the socket handle instantly cancels any in-flight `select()` or `send()` calls, allowing worker threads to exit in sub-millisecond time without blocking `systhread_join()`.

2. **Reduce Telemetry Blocking Timeouts**:
   In `perform_send()`, reduce the `select()` retry timeout from `tv.tv_sec = 1` (1000ms) to a non-blocking timeout of `tv.tv_usec = 10000` (10ms), or drop unsent telemetry frames immediately when socket buffers are full.

3. **Persistent Winsock State**:
   Keep Winsock initialized (`WSAStartup`) for the entire duration of the Max process lifetime instead of tearing down Winsock when `ref_count` hits 0 during temporary scripting patcher reloads.

4. **Timeouts on Shared Mutex Locks**:
   Replace `WaitForSingleObject(g_hSharedMutex, INFINITE)` with a bounded timeout (e.g., `WaitForSingleObject(g_hSharedMutex, 100)`). If the wait times out, fall back safely rather than freezing Max.

# Technical Report: Crucible Object to Visualize Module Handoff & Packet Transmission Lifecycle

## 1. Executive Summary
This report provides a detailed software engineering analysis of the handoff lifecycle of completed spans inside the `crucible` external object to the `visualize` module, which handles the serialization and transmission of TCP packets to the `visualizer.py` rendering application.

Additionally, this report diagnoses and explains two highly specific and subtle behaviors observed in the visualizer communication loop:
1. **Why `repopulate` packets fail to be received by the visualizer during new span wins—regardless of their size (even when extremely small)—but succeed during the `rebar` message execution, all without triggering any errors or verbose logging.**
2. **Why `new_span` packets successfully make it to the visualizer but also fail to trigger any logging messages in the Max console.**

We demonstrate that these phenomena are caused by a combination of Max's headless object logging suppression, a non-blocking connection handshake race condition on back-to-back queued packets, and TCP window exhaustion under non-blocking sockets.

---

## 2. The Completed Span Handoff Lifecycle & Data Flow

The lifecycle of a completed span transitions through four distinct layers in the system architecture:

```
[ Max MSP / rebar ]
       │  (track / span messages)
       ▼
[ crucible / rebar_crucible_internal ]
       │  (crucible_process_span)
       ▼
[ shared/visualize module ]
       │  (enqueue via thread-safe FIFO queue)
       ▼
[ Background viz_worker_thread ]
       │  (non-blocking WinSock2 TCP Socket)
       ▼
[ visualizer.py (Port 9999) ]
```

### Step 2.1: Evaluation and Mutex/Thread Context
A span is completed inside the `buildspans` object and emitted through its outlets. In a coordinated `rebar` instance, these outlets are intercepted, passing the `track` ID and `span` list directly to `crucible` (or the headless internal `rebar_crucible_internal` module).
- **Thread Model:** If `@async` is enabled, `crucible_anything` intercepts the messages and enqueues them onto a background worker thread (`x->worker`) managed by the `shared/async_worker` module. If `@async` is disabled, processing happens synchronously on the main thread.
- **Sequential Safety:** In either setting, `track` and `span` messages are executed in strict chronological order. `crucible_process_span` is called on the active thread to evaluate whether the challenger's rating is superior to the incumbent's rating for the specified bars.

### Step 2.2: Serialization and Packet Assembly
If the challenger wins, the incumbent transcript dictionary is updated. If the `@visualize` attribute is enabled, the object immediately initiates visualizer transmission:
1. **Repopulate Event:** `crucible_visualize_repopulate` is invoked. It performs a deep recursive serialization of the entire incumbent dictionary using a custom dynamic string builder (`t_dyn_str`), converting the dictionary tree into a single large JSON string.
2. **New Span Event:** `crucible_visualize_state` is invoked with the `new_span` event type. This builds a highly compact JSON payload containing only the metadata of the newly won span (such as track ID, bar timestamps, and the winning rating).

### Step 2.3: Enqueuing in the Thread-Safe FIFO Queue
Both JSON payloads are passed to `visualize(void *x, const char *message)`:
- `visualize` resolves the class name of `x` to determine the target TCP port (Port 9999 for `crucible` and Port 8999 for `weaver~`).
- It locks the global queue mutex (`queue_mutex`) and appends a newly allocated `t_viz_queue_item` containing the serialized payload to a thread-safe FIFO queue.
- It signals the condition variable `viz_cond` to wake up the background `viz_worker_thread` and releases the mutex.

### Step 2.4: TCP Transmission via Background Worker Thread
The background `viz_worker_thread` wakes up, pops the item, locks the socket mutex, and calls `perform_send`:
- **Connection Handshake:** It calls `ensure_connected`, which initializes a TCP stream socket and puts it into non-blocking mode (`FIONBIO`).
- **Framing:** It prefixes the packet with the routing metadata `{"type":"crucible", ...` and appends a trailing `\n` newline character for packet framing.
- **Non-blocking Write Loop:** It enters a loop calling WinSock's `send` to transmit the buffer over Port 9999.

---

## 3. Why `repopulate` Packets Fail to Be Received on New Span Wins (Even When Small)

We identify two independent, compounding root causes explaining why `repopulate` packets fail to be received by the visualizer during real-time span wins:

### Root Cause 3.1: The Asynchronous TCP Handshake Race Condition (Why Small Packets Fail)
When a new span is won, the `visualize` module is typically in a disconnected state (`vs->sock == INVALID_SOCKET`). The `repopulate` packet and the `new_span` packet are enqueued **back-to-back in rapid succession** (less than 1ms apart):

1. **`repopulate` Processing:**
   - The worker thread pops the first packet (`repopulate`) and calls `perform_send`.
   - `perform_send` calls `ensure_connected`.
   - `ensure_connected` creates the socket and initiates a non-blocking connection via `connect()`. Since the socket is non-blocking, `connect()` immediately returns `SOCKET_ERROR` with the error `WSAEWOULDBLOCK` (or `WSAEINPROGRESS`), indicating the TCP three-way handshake has been initiated asynchronously but is not yet complete.
   - `perform_send` immediately proceeds to call `send()`.
   - Because the TCP handshake is still ongoing in the background, `send()` immediately fails with `SOCKET_ERROR` and the error code `WSAENOTCONN` or `WSAEWOULDBLOCK` (sending 0 bytes).
   - Because `total_sent == 0`, the socket-closure condition (`if (total_sent > 0) closesocket(...)`) is **not** triggered. The socket remains open but half-connected, and the first packet (`repopulate`) is discarded.

2. **`new_span` Processing:**
   - The worker thread immediately pops the second packet (`new_span`) and calls `perform_send`.
   - `ensure_connected` is called, but since `vs->sock != INVALID_SOCKET`, it returns immediately.
   - By this time (a few milliseconds/ticks later), **the asynchronous TCP handshake has successfully finished** in the background, making the socket fully active.
   - `send()` succeeds immediately on the now-connected socket, sending the tiny `new_span` packet successfully!

This race condition guarantees that **the very first packet enqueued when the socket is closed (which is always the `repopulate` packet) will ALWAYS fail, regardless of its size—even if it is extremely small (e.g., 50 bytes)**.

### Root Cause 3.2: TCP Window Exhaustion (Why Large Packets Fail)
For large incumbent dictionaries (typical session files), the `repopulate` packet size can range from **100KB to several megabytes**. Even if the handshake has already completed, sending this giant buffer over a non-blocking socket saturates the TCP window.
- Once saturated, `send` returns `SOCKET_ERROR` with `WSAEWOULDBLOCK`.
- Since `total_sent > 0` (some chunks were successfully written), the socket is immediately closed:
  ```c
  if (total_sent > 0) {
      closesocket(vs->sock);
      vs->sock = INVALID_SOCKET;
  }
  ```
- This hard-aborts the TCP stream mid-transmission, discarding the trailing JSON framing (`\n`). The receiver (`visualizer.py`) receives a truncated chunk, fails to parse it, and ignores it.
- When the next packet (`new_span`) is popped, `vs->sock` is `INVALID_SOCKET`, so it reconnects, waits, and sends successfully.

---

## 4. Why `new_span` Packets Succeed and Why All Logging Is Silent

### Why `new_span` Packets Succeed
As analyzed in Step 3.1:
- If the socket was closed due to buffer exhaustion (`total_sent > 0`), the next `new_span` packet triggers a brand new connection via `ensure_connected`.
- Since the worker thread handles other deferred queue tasks or experiences tiny CPU scheduling gaps, the handshake completes by the time `new_span` calls `send()`.
- Because the `new_span` payload is extremely lightweight (a few dozen bytes), it easily fits in a single TCP window write and never triggers `WSAEWOULDBLOCK`, landing intact at the visualizer.

### Why Logging is Completely Silent (Headless CLASS_NOBOX Suppression)
The lack of verbose logs in the Max console for these connection resets and packet transmissions is explained by Max MSP's internal object and log routing architecture:
1. **The Object Pointer (`x`):** The pointer `x` passed to `visualize` and logged via `object_post` represents the `rebar_crucible_internal` object when running inside the coordinated `rebar` orchestrator.
2. **Headless Class Registration:** The internal modules of the `rebar` object are registered using a private interceptor `rebar_intercept_class_new`, which registers them with Max's core as **`CLASS_NOBOX`** (headless, non-graphical box-less classes).
3. **Max Console Formatting Suppression:** In Max, the standard `object_post` and `object_error` APIs expect the target object pointer to belong to a graphical box (`CLASS_BOX`). When `object_post` is called with a `CLASS_NOBOX` object, Max's internal console system fails to resolve a graphical patcher box association for the object. To prevent clutter and potential null pointer formatting dereferences, **Max silently discards or suppresses all `object_post` messages originating from `CLASS_NOBOX` objects.**
4. **Consistency Across Threads:** This suppression is a property of the class type and object registration, which is why the logging is **completely silent regardless of whether `@async` is enabled or disabled**.

In contrast, when a `rebar` message is sent to a standalone `crucible` object, the target is a standard `CLASS_BOX` object. Max successfully associates the object with its box in the patcher, allowing all `object_post` messages (including connection notifications and `repopulate` statuses) to print perfectly in the Max console.

---

## 5. Architectural Recommendations

To resolve these communication overheads and packet losses in future updates, we recommend the following structural adjustments:
- **Synchronous Connection Handshakes:** `ensure_connected` should poll the socket using `select()` with a short timeout to wait for the handshake to finish before allowing `perform_send` to proceed, ensuring the first packet is never dropped.
- **Rate Limiting / Debouncing:** Rather than sending a complete `repopulate` packet on *every single* won span during rapid-fire operations, the object should implement a short debounce timer (e.g. 50ms) to trigger a single, consolidated state synchronization.
- **Adaptive Buffer Sizes & Chunking:** The `perform_send` loop should be modified to handle non-blocking `WSAEWOULDBLOCK` delays gracefully using standard socket polling (`select` or `WSASend` with completion events) instead of immediately closing the socket.
- **Log Redirection:** Logging inside the headless modules should utilize the consolidated common logger (`crucible_log` which writes to `x->log_outlet`) rather than `object_post`, ensuring that internal diagnostic messages are successfully routed out of the parent `rebar` object's log outlet.

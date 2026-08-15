# Speculative Architectural Report: RayLib Visualizer TCP Packet Cross-Routing in `bpatcher` Abstractions

**Author:** Jules (Software Engineer)
**Target Component:** `analyze~` / `mc.analyze~` External & RayLib Companion Visualizer Framework
**Scope:** Investigation of packet routing anomalies, port allocation races, and abstraction state collisions in Max/MSP `bpatcher` environments.

---

## Executive Summary

When deploying multiple `analyze~` or `mc.analyze~` objects within Max/MSP abstractions (specifically inside `bpatcher` UI containers), a critical symptom may arise: despite each object appearing to spawn its own separate RayLib companion visualizer window on distinct TCP ports (as shown in the window title bar or GUI attributes), all `analyze~` instances seem to send their telemetry TCP packets to the **same** visualizer process, or display identical telemetry data across windows.

This report presents a thorough speculative analysis of the underlying mechanisms driving this issue. Based on a deep-dive review of `analyze~.c`, `mc.analyze~.c`, `shared/visualize.c`, and `python/transience_vis.py`, we identify **five core technical factors** responsible for this behavior:

1. **Port Allocation Race Conditions During Bulk Abstraction Loading** (Socket reuse / TIME_WAIT windows between `bind()` test and Python server initialization).
2. **Shared Group Buffer (`@group`) Telemetry Payload Identity** (Visual deception where separate TCP sockets send identical underlying C buffer data).
3. **Abstraction Scripting Name (`varname`) Collisions** (Identity ambiguity across identical `bpatcher` subpatchers).
4. **Fallback Socket Routing via `get_socket_for_object` in `shared/visualize.c`** (Hardcoded default singletons routing traffic to port 9001).
5. **Asynchronous Queue Pointer Lookup in `shared/visualize.c`** (Thread-safe socket lookup timing across dynamically allocated ports).

---

## Architectural Context

Each `analyze~` external instance (or channel in `mc.analyze~`) manages its visual telemetry pipeline through three distinct layers:

```
+-------------------------------------------------------------------+
|                        Max / MSP Patcher                          |
|  +---------------------------+     +---------------------------+  |
|  |   bpatcher Instance #1    |     |   bpatcher Instance #2    |  |
|  |  +---------------------+  |     |  +---------------------+  |  |
|  |  |      analyze~       |  |     |  |      analyze~       |  |  |
|  |  | (x->viz_port: 9001) |  |     |  | (x->viz_port: 9002) |  |  |
|  |  +----------+----------+  |     |  +----------+----------+  |  |
|  +-------------|-------------+     +-------------|-------------+  |
+----------------|---------------------------------|----------------+
                 | (JSON-over-TCP)                 | (JSON-over-TCP)
                 v                                 v
+-------------------------------------------------------------------+
|                     shared/visualize.c Engine                     |
|  - Dynamic Socket Array: dynamic_sockets[MAX_DYNAMIC_SOCKETS]     |
|  - Shared Port Map Memory: Local\MaxAnalyzeVizSharedPorts         |
|  - Asynchronous Worker Thread: viz_worker_thread                  |
+-------------------------------------------------------------------+
                 |                                 |
                 v 127.0.0.1:9001                  v 127.0.0.1:9002
+-------------------------------------------------------------------+
|                   Python RayLib Companion Processes               |
|  +---------------------------+     +---------------------------+  |
|  |   transience_vis.py #1    |     |   transience_vis.py #2    |  |
|  |     (Listening: 9001)     |     |     (Listening: 9002)     |  |
|  +---------------------------+     +---------------------------+  |
+-------------------------------------------------------------------+
```

---

## Detailed Root Cause Analysis

### 1. Port Allocation Race Conditions During Bulk Abstraction Loading

When a Max patch containing multiple `bpatcher` objects is opened, Max instantiates all embedded `analyze~` objects almost simultaneously during patch layout construction and DSP graph compilation (`dsp64` method).

In `shared/visualize.c`, port allocation is performed by `visualize_allocate_port(start_port)`:

```c
SOCKET test_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
if (test_sock != INVALID_SOCKET) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.S_un.S_addr = inet_addr(SERVER);

    int res = bind(test_sock, (struct sockaddr*)&addr, sizeof(addr));
    closesocket(test_sock);

    if (res == 0) {
        // Port is deemed free -> registered in shared port map
        return port;
    }
}
```

#### The Race Mechanism:
1. **Instance #1** calls `visualize_allocate_port(9001)`. The C code creates `test_sock`, binds to `9001`, gets `res == 0`, closes `test_sock`, registers `9001` in `dynamic_sockets` and `Local\MaxAnalyzeVizSharedPorts`, and calls `CreateProcessA` to launch `python transience_vis.py --port 9001`.
2. **Instance #2** inside the second `bpatcher` initializes **milliseconds later**—*before* the Python interpreter for Instance #1 has fully booted and bound its TCP server socket on port 9001.
3. If `Local\MaxAnalyzeVizSharedPorts` is checked across process boundaries, it should block port `9001`. However, if Instance #1 and Instance #2 share the same process space (as DLL instances inside Max), `is_port_in_shared_map(9001)` prevents reuse *only if* the shared memory table updated fast enough.
4. If a socket `bind()` test occurs after `closesocket(test_sock)`, Winsock places port 9001 in a short `TIME_WAIT` or deferred release state. If `CreateProcessA` fails or takes >500ms to spawn Python, Instance #2 might receive port 9001 or 9002 incorrectly.
5. Even worse: if Instance #2 spawns Python on `--port 9001` because Instance #1's Python process hasn't bound yet, Instance #2's Python process will fail to bind when it starts up (`bind: address already in use`). Instance #2's Python process terminates immediately.
6. Now, **only one** Python visualizer process (Instance #1) remains alive on port 9001. If Instance #2's C struct somehow fell back or re-routed packets to port 9001, both objects will transmit to the single active visualizer on port 9001!

---

### 2. Shared Group Buffer (`@group`) Payload Homogeneity (Visual Illusion)

In `analyze~` and `mc.analyze~`, transient metrics can be synchronized across multiple instances using the `@group` attribute:

```c
// analyze~.c / mc.analyze~.c
CLASS_ATTR_SYM(c, "group", 0, t_analyze, group_name);
```

When objects share a group name (e.g., `@group main` or a default group name inside an abstraction like `@group #1`), they share a single underlying `SharedTransientBuffer` structure registered in Max's `CLASS_NOBOX` registry (`analyze_shared_buffer`).

#### The Consequence:
- The DSP thread in Instance #1 writes audio peaks to the shared buffer.
- The DSP thread in Instance #2 writes audio peaks to the **same** shared buffer.
- When Instance #1 sends its JSON telemetry packet to Port 9001, it sends `accumulated_buffer`, `rating`, `std_dev`, and `peaks` from the shared group buffer.
- When Instance #2 sends its JSON telemetry packet to Port 9002, it sends the **exact same** `accumulated_buffer`, `rating`, `std_dev`, and `peaks` from the shared group buffer!

**Result:** Both RayLib visualizer windows (Port 9001 and Port 9002) display identical graphs, waveforms, and ratings. To the user, it appears that both `analyze~` objects are sending their data to a single visualizer window, when in reality separate packets are being delivered to separate ports with identical payloads!

---

### 3. Abstraction Scripting Name (`varname`) Ambiguity in `bpatcher`

When `launch_visualizer` constructs the process launch command and JSON header, it retrieves the object's scripting name:

```c
t_symbol *s_name = object_attr_getsym(x, gensym("varname"));
const char *scripting_name = (s_name && s_name != gensym("")) ? s_name->s_name : "";
```

Inside a `bpatcher`, objects inside the abstraction patcher do not have unique scripting names (`varname`) unless explicitly set in the Max inspector or dynamically assigned via patcher scripting.

#### The Consequence:
1. If two `bpatcher` instances load `my_abstraction.maxpat`, both internal `analyze~` objects have `scripting_name == ""`.
2. When `transience_vis.py` updates its window title via `build_title()`, both windows display identical title strings:
   `Cumulative Transience Real-Time Visualizer (Port 9001)`
   `Cumulative Transience Real-Time Visualizer (Port 9002)`
3. If scripting names are omitted, packet identification inside RayLib cannot differentiate between instances based on patcher hierarchy or parent `bpatcher` name.

---

### 4. Static Fallback Routing via `get_socket_for_object` in `shared/visualize.c`

`shared/visualize.c` provides two packet dispatch functions:
1. `visualize(x, message)` — Uses `get_socket_for_object(x)` to find a static socket.
2. `visualize_to_port(x, port, type, message)` — Uses dynamic port lookup in `dynamic_sockets`.

In `analyze~.c`:
```c
visualize_to_port(x, x->viz_port, "analyze", json_buf);
```

However, if `x->viz_port` is 0 or if `visualize_to_port` fails to resolve `vs` for `port`, or if any code path calls the generic `visualize(x, msg)` helper, `get_socket_for_object` executes:

```c
static t_viz_socket *get_socket_for_object(void *x, const char **type_out) {
    ...
    } else if (classname == gensym("analyze~") || classname == gensym("mc.analyze~")) {
        if (type_out) *type_out = (classname == gensym("mc.analyze~")) ? "mc_analyze" : "analyze";
        return &analyze_viz; // <--- HARDCODED TO PORT 9001 (PORT_ANALYZE)
    }
    return NULL;
}
```

If `x->viz_port` evaluates to 0 (for example, if `launch_visualizer` failed or was skipped, or during re-initialization), any fallback to `visualize()` will force packets from **all** `analyze~` instances directly to `analyze_viz` (Port 9001).

---

### 5. Asynchronous Queue Pointer Lookup in `shared/visualize.c`

When `visualize_to_port` is called, it places a `t_viz_queue_item` onto `queue_head`:

```c
t_viz_queue_item *item = (t_viz_queue_item *)sysmem_newptr(sizeof(t_viz_queue_item));
item->vs = vs; // <--- Pointer to entry in dynamic_sockets array
item->x = x;
```

`dynamic_sockets` is a static array of size `MAX_DYNAMIC_SOCKETS` (64):

```c
typedef struct {
    int port;
    t_viz_socket vs;
    int in_use;
} t_dynamic_socket;

static t_dynamic_socket dynamic_sockets[MAX_DYNAMIC_SOCKETS];
```

If `mc_analyze_free` or `analyze_free` is called when a `bpatcher` is closed or reloaded, `visualize_close_port(port)` marks `dynamic_sockets[i].in_use = 0` and closes the socket. If the background `viz_worker_thread` still has pending queue items referencing `item->vs`, it may send packets to a recycled or re-allocated socket slot that has been reassigned to a different port!

---

## Diagnostic Matrix

| Symptom | Primary Cause | Affected Code / Module |
| :--- | :--- | :--- |
| **Both windows show identical graphs & ratings** | `@group` attribute sharing a single `SharedTransientBuffer` | `analyze~.c`, `mc.analyze~.c`, `cumulative_transience.c` |
| **Only 1 window opens despite multiple `bpatcher`s** | Port allocation race condition during `bind()` test; 2nd Python process fails to bind and exits | `shared/visualize.c` (`visualize_allocate_port`), `transience_vis.py` |
| **Window titles are indistinguishable** | Blank `varname` inside abstractions | `analyze~.c` (`launch_visualizer`), `transience_vis.py` (`build_title`) |
| **Packets all land on Port 9001** | `x->viz_port == 0` causing fallback to `analyze_viz` singleton | `shared/visualize.c` (`get_socket_for_object`), `analyze~.c` |

---

## Recommended Architectural Solutions

### Solution 1: Implement Parent Patcher + Box Hierarchy Scripting Name Resolution
To ensure unique identification inside abstractions, query both the parent box (`#B`) and parent patcher (`#P` / `bpatcher`) names when `varname` is not explicitly set:

```c
t_symbol *s_name = object_attr_getsym(x, gensym("varname"));
if (!s_name || s_name == gensym("")) {
    // Lookup parent box in patcher hierarchy
    t_object *box = NULL;
    object_obex_lookup(x, gensym("#B"), &box);
    if (box) {
        s_name = object_attr_getsym(box, gensym("varname"));
    }
}
```

### Solution 2: Atomic Port Allocation with Keep-Alive Socket Reservation
Modify `visualize_allocate_port` so that instead of opening and immediately closing `test_sock`, the socket remains bound until the companion process acknowledges connection, or enforce a strict inter-process port allocation lock with retry logic:

```c
// Retain bound socket or pass socket handle, preventing port allocation race conditions
```

### Solution 3: Isolate `@group` Buffers Across Abstraction Instances
Ensure that abstractions pass unique arguments (e.g., `#0` or `#1` in Max) to `@group` attributes (e.g., `@group #0_transient`), so that distinct `bpatcher` instances maintain independent transient analysis history buffers unless explicitly intended to be linked.

### Solution 4: Safeguard `visualize_to_port` Against Port 0 Fallback
Add an explicit guard in `analyze_worker_task` / `mc_analyze_worker_task`:

```c
if (x->visualize_enabled && x->viz_port > 0) {
    visualize_to_port(x, x->viz_port, "analyze", json_buf);
} else {
    // Do NOT fall back to static analyze_viz socket!
}
```

---

## Conclusion

The observed phenomenon—where `analyze~` objects in `bpatcher` abstractions appear to send packets to the same RayLib visualizer—is caused by a combination of **port allocation races during patch loading**, **`@group` buffer data sharing**, and **blank scripting name identities in Max abstractions**. Applying the recommended safeguards will guarantee robust multi-instance visualizer isolation across complex `bpatcher` patch hierarchies.

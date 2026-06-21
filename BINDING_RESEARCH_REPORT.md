# Research Report: "Binding" `buildspans` to `crucible`

## Overview
This report explores the technical feasibility of implementing a "binding" mechanism between the `buildspans` and `crucible` objects in the Max environment. The goal is to allow these two objects to communicate directly without relying on patch cords, similar to how a `pattr` object binds to its target. This would facilitate tighter coordination and support future asynchronous architectures.

## Technical Feasibility
Binding standalone objects in Max is entirely possible using the Max SDK. We have already implemented a "tightly coupled" version of this coordination within the `rebar` container object, which intercepts virtual outlet calls from `buildspans` and routes them directly to the `crucible` module. Extending this to standalone objects requires two main components: **Object Discovery** and **Lifecycle Management**.

### 1. Object Discovery Strategies
For a `buildspans` object to bind to a `crucible`, it must first find it within the Max patcher.
*   **Scripting Names (varname):** The most robust method is to provide `buildspans` with a `@bind [name]` attribute. The object can then use `object_findregistered` or traverse the patcher hierarchy using `jpatcher_get_firstobject()` to locate the `crucible` object with that specific scripting name.
*   **Patch-Cord Discovery:** As suggested, a patch cable could serve as the "binder." In the SDK, `buildspans` can traverse its outlet's connection list. By identifying that a `crucible` object is connected to its first outlet, `buildspans` can "promote" the relationship, storing a direct pointer to the `crucible` and bypassing the standard `outlet_anything` stack for subsequent messages.
*   **Class-based Traversal:** Alternatively, `buildspans` could search its own patcher for any instance of the `crucible` class, though this is less precise if multiple instances exist.

### 2. Lifecycle Management and Safety
Maintaining a raw pointer to another object is dangerous in Max because the user can delete objects at any time.
*   **`object_attach` & `object_notify`:** The SDK provides a notification system. By attaching to the `crucible`, `buildspans` would receive a `free` notification if the `crucible` is deleted. This allows `buildspans` to nullify its pointer safely, preventing a crash.

### 3. Direct Message Transmission
Once a valid pointer is obtained, `buildspans` can bypass the outlet system entirely:
*   **Direct Method Invocation:** Instead of `outlet_anything`, `buildspans` would call `object_method()` or `typedmess()` on the `crucible` pointer.
*   **Header Inclusion:** If both objects are part of the same project, `buildspans` could even include the internal headers of `crucible` to call specific processing functions directly, though this increases code coupling.

## Benefits for Asynchrony
The user suggested that binding could enable both objects to stay fully asynchronous yet coordinated.

### Thread-Safe Communication
Currently, these objects communicate via the Max message thread (Main Thread). If we move to an asynchronous model:
*   **Shared Memory / Dictionaries:** Both objects already rely heavily on `t_dictionary`. A bound relationship allows them to share pointers to critical sections or mutexes more easily.
*   **Asynchronous Queues:** `buildspans` could push completed spans into a lock-free FIFO queue owned by the bound `crucible`. The `crucible` could then process these on a background worker thread (similar to the `shared/visualize` module) without blocking the Max UI or audio threads.

## Challenges
*   **Implicit vs. Explicit Logic:** Bypassing patch cords makes the logic of a patch harder to read for other users. Proper visualization and logging (already a priority in this codebase) would be essential to indicate the active binding.
*   **Namespace Collisions:** Ensuring that `@bind` targets the correct instance in complex nested subpatchers requires careful implementation of patcher depth traversal.

## Conclusion
Binding `buildspans` to `crucible` is a viable and recommended path for optimizing the "building" workflow. It reduces message overhead, increases safety through the `object_attach` mechanism, and provides a solid foundation for offloading heavy dictionary processing to background threads.

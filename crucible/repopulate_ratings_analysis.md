# Analysis of Repopulate Package Rating Discrepancies Across Crucible Instances

## 1. Executive Summary
An issue has been identified where new ratings sent via a `replace` message to a `crucible` object are conveyed in the visualizer `repopulate` telemetry package emitted by that instance, yet fail to persist in the shared transcript dictionary. Consequently, when a second `crucible` object emits a `repopulate` package after processing ordinary incoming musical spans, the ratings revert to their original or un-replaced values.

This report presents a thorough technical investigation of the underlying causes in `crucible.c`, outlining the operational differences between `replace` and `span` processing, detailing five key mechanisms driving the discrepancy, comparing the serialized JSON packages, and offering actionable remedies.

---

## 2. Architectural Overview of Crucible Objects and Dictionaries
In the system architecture, `crucible` objects manage competitive evaluation of musical spans across multiple audio tracks. Each `crucible` instance interacts with two primary dictionaries:
1. **Challenger Dictionary (`challenger_dict`)**: A private per-object dictionary storing incoming bar selectors, spans, offsets, palettes, and ratings as they are assembled.
2. **Incumbent Dictionary (`incumbent_dict`)**: A shared global Max dictionary (registered with `dictobj` under a name such as `transcript`) containing the current winning timeline of bars, spans, and ratings across all tracks.

Multiple `crucible` instances in a Max patcher can be bound to the same shared `incumbent_dict` name.

---

## 3. Discrepancy Mechanisms: Why Replaced Ratings Are Lost

### Cause 1: Challenger Dictionary Overwrite on Subsequent Winning Spans
When **Instance A** receives a `replace` message (e.g., `replace track_1::bar_100::rating 8.5`):
- Instance A looks up `track_1` and `bar_100` inside `incumbent_dict`.
- Instance A updates the `rating` key of `bar_100` in `incumbent_dict`.
- Instance A calls `crucible_visualize_repopulate`, which reads `incumbent_dict` and broadcasts the new rating in its `repopulate` telemetry packet.

However, **Instance A does NOT update `challenger_dict`** in its own instance or in any other `crucible` instance (such as **Instance B**).

When **Instance B** later receives an ordinary `span` message for `track_1`:
- Instance B evaluates the span from its `challenger_dict` against `incumbent_dict`.
- If the challenger span wins, Instance B copies the winning bar dictionaries from `challenger_dict` into `incumbent_dict` using `dictionary_deep_copy(challenger_bar_dict)`.
- Because `challenger_bar_dict` was populated from raw incoming bar messages and was never modified by the `replace` event, copying it into `incumbent_dict` **completely overwrites `bar_100`**, restoring the stale pre-replace rating.
- Instance B then emits a `repopulate` packet containing the overwritten original rating.

### Cause 2: Missing Max `dictobj_changed` Notifications
In the Cycling '74 Max MSP C SDK, dictionaries registered with `dictobj` reside in shared memory. Modifying a C `t_dictionary` struct in memory via functions like `dictionary_appendatom` updates the C data structure directly.

However, Max's internal notification system and any Max `dict` UI objects bound to that dictionary name are **only notified of mutations if `dictobj_changed(x->incumbent_dict_name)` is called**.

In `crucible.c`, `s == gensym("replace")` modifies `incumbent_dict` directly but does not invoke `dictobj_changed`. As a result:
- Instance A's local `repopulate` serializer reads the C struct directly from memory, successfully displaying the updated rating in telemetry.
- Any Max `dict` object, file saver, or external query inspecting the registered transcript dictionary from the Max patcher environment still observes the pre-replace state.

### Cause 3: Silent Drop on Non-Existent Bars
If a `replace` message is sent for a bar that has not yet entered `incumbent_dict` (i.e., `specified_bar_dict` is `NULL` because no winning span has created that bar in `incumbent_dict` yet):
- The mutation block inside `crucible.c` checks `if (specified_bar_dict)` and skips the update when `specified_bar_dict` is `NULL`.
- The rating change is silently discarded without being written to `incumbent_dict`.
- However, if `@visualize` is enabled, the execution falls through to `crucible_visualize_repopulate` and emits a telemetry event packet with the requested rating.
- When that bar later enters `incumbent_dict` via an ordinary `span`, it receives its default challenger rating, making it appear that `replace` failed to take effect.

### Cause 4: Lack of Cross-Instance Thread Synchronization
When `@async 1` or `@defer 1` is enabled:
- Each `crucible` instance executes tasks on its own background thread worker using an instance-specific mutex (`x->state_mutex`).
- There is no shared global mutex synchronizing access to `dictobj` registered dictionaries across separate `crucible` instances.
- If Instance A processes a `replace` message concurrently while Instance B processes an incoming `span` message, race conditions occur where Instance B reads or overwrites `incumbent_dict` before or during Instance A's modification.

### Cause 5: Partial Span Coverage with Span Rating Averaging (`@meld`)
When `@meld` is enabled on `crucible`:
- Receiving a `replace` message calculates an average rating across all bars currently constituting the span in `incumbent_dict`.
- If only a subset of bars in a multi-bar span are present in `incumbent_dict` at the moment `replace` arrives, only those present bars receive the updated melded rating.
- When remaining bars of the span subsequently enter `incumbent_dict` via an ordinary `span` message from Instance B, those new bars enter with their original challenger ratings rather than the melded rating.

---

## 4. Comparison of Repopulate Telemetry Packages

Both `replace` and `span` events trigger the function `crucible_visualize_repopulate_ex`, which generates a JSON payload structured as follows:

```json
{
  "event": "repopulate",
  "bar_length": 1920,
  "dictionary": {
    "1": {
      "0": {
        "palette": "palette_1.wav",
        "offset": 0.0,
        "span": [0, 1920, 3840],
        "scores": [0.85, 0.92, 0.78],
        "absolutes": [120, 1940, 3860],
        "rating": 8.5
      }
    }
  }
}
```

### Key Payload Differences Between Instances

1. **Rating Values (`rating`)**:
   - **Instance A (`replace`)**: Contains updated float values (e.g., `"rating": 8.500000`) for modified bars because Instance A reads the modified C struct directly after editing `specified_bar_dict`.
   - **Instance B (`span`)**: Contains original un-replaced float values (e.g., `"rating": 4.200000`) because Instance B's winning span overwrote `incumbent_dict` using un-replaced `challenger_dict` data.

2. **Presence of Rebar Flag (`rebar`)**:
   - Standard `repopulate` packages (sent after `replace` or `span`) do not include the `"rebar": true` key-value pair.
   - `repopulate` packages triggered during time signature transformation include `"rebar": true`.

3. **Subsequent Event Telemetry Packets**:
   - Following `repopulate`, **Instance A** sends a `replace` event packet:
     `{"event":"replace","track":"1","bar":"0","rating":8.500000,"principal":true}`
   - Following `repopulate`, **Instance B** sends a `new_span` event packet:
     `{"event":"new_span","track":"1","bar":"0", ...}`

---

## 5. Recommended Technical Remedies

1. **Update Challenger State or Persistent Overrides**:
   Modify `crucible_do_anything` so that when a `replace` message is processed, the new rating is written to `challenger_dict` as well as `incumbent_dict`, or recorded in a persistent `rating_overrides` table that is consulted whenever new spans are copied into `incumbent_dict`.

2. **Invoke `dictobj_changed()`**:
   Add a call to `dictobj_changed(x->incumbent_dict_name)` immediately after modifying `incumbent_dict` during `replace` handling to ensure Max `dict` objects and patcher listeners stay synchronized.

3. **Auto-Create Missing Bar Entries on `replace`**:
   When `specified_bar_dict` is `NULL`, automatically create the necessary track and bar sub-dictionaries in `incumbent_dict` before writing the rating, preventing silent drops of `replace` messages on new bars.

4. **Implement Shared Mutex Protection**:
   Introduce a shared global mutex for `incumbent_dict` access across all `crucible` instances to prevent asynchronous race conditions during concurrent `span` and `replace` execution.

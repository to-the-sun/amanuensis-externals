# Technical Report: Palette Buffer Read Position Calculation and Audio Playback Mechanics in weaver~

## Executive Summary

The `weaver~` object in Max MSP is a real-time audio synthesis and composition streaming engine. It reads control and timestamp information from a signal ramp, detects bar boundaries, queries a Max dictionary transcript (such as `transcript.json`), and streams slice audio from source palette buffers (from `polybuffer~` or `buffer~` objects) into destination audio channels.

This report provides a complete breakdown of how `weaver~` calculates the exact sample position in a palette buffer to read from, written in plain English and straightforward formulas without TeX or math-mode symbols. Furthermore, it presents a detailed analysis of potential failure modes and architectural edge cases that can cause audio to be played incorrectly (or missed entirely) even when the transcript dictionary contains correct and valid values.

---

## 1. System Architecture & Thread Lifecycle

The `weaver~` system operates across two main execution domains:
1. **The High-Priority Audio/DSP Thread (`weaver_perform64` / `weaver_process_vector`)**: Runs in real time per vector, evaluates the signal ramp, updates crossfade envelopes, interpolates sample lookups, and writes into destination buffer memory.
2. **The Low-Priority Main/GUI Thread (`weaver_audio_qtask` via `qelem`)**: Handles thread-safe dictionary lookups (via `dictobj`), buffer binding checks (via `buffer_ref`), key parsing, and state handover back to the DSP thread.

```
+-----------------------------------------------------------------------------------+
|                                 DSP THREAD                                        |
|  1. Ramp Input -> Track Scan Position (in milliseconds)                           |
|  2. Bar Hit Detection (Integer Division & Floor Rounding)                         |
|  3. Enqueue Hit Event into Lock-Free Circular FIFO Queue                           |
+-----------------------------------------------------------------------------------+
                                          |
                                          v (via qelem_set / audio_qtask)
+-----------------------------------------------------------------------------------+
|                                MAIN THREAD                                        |
|  4. Dequeue Hit Event from FIFO                                                   |
|  5. Format Bar Key String ("0", "2000", etc.)                                     |
|  6. Query Transcript Dictionary                                                   |
|  7. Extract "palette", "offset", and "rating" values                              |
|  8. Validate Buffer Binding & Update Pending Track Metadata                       |
+-----------------------------------------------------------------------------------+
                                          |
                                          v (Handover under lock)
+-----------------------------------------------------------------------------------+
|                                 DSP THREAD                                        |
|  9. Swap Crossfade Slot Active State (Slot 0 <-> Slot 1)                           |
| 10. Store Slot Read Offset: Active Slot Offset = Dict Offset                      |
| 11. Compute Sample Index: Source Time (ms) = Active Slot Offset + Current Time    |
| 12. Linear Fractional Sample Interpolation                                        |
| 13. Dynamic Gain & Crossfade Ramp Processing                                      |
| 14. Write Interleaved Output to Destination Buffer                                |
+-----------------------------------------------------------------------------------+
```

---

## 2. Step-by-Step Calculation & Plain English Guide

### Step 1: Signal Ramp Normalization and Track Time
At any given sample frame within a DSP vector, `weaver~` reads an incoming millisecond signal ramp and calculates the global song scan time in milliseconds:

Scan Time = Signal Ramp Value + Most Negative Bar Offset

Where:
- Signal Ramp Value is the raw incoming signal ramp (in milliseconds).
- Most Negative Bar Offset is the start timestamp of the earliest bar in the song transcript (for example, -2000.0 ms for a song with a 1-bar pickup, or 0.0 ms if there is no pickup).

### Step 2: Continuous Bar Hit Detection and Bar Start Boundaries
Let Bar Length be the bar duration in milliseconds (retrieved from the first sample of the "bar" buffer, such as 2000.0 ms for 120 BPM).

During vector processing, `weaver~` tracks the scan time converted into integer milliseconds. When a bar boundary is crossed, `weaver~` determines the theoretical bar start timestamp (referred to in code as `latest_j` or `rel_time`).

For positive scan times:
Bar Start Time = (Scan Time / Bar Length) * Bar Length

For negative scan times (such as pickup bars):
Bar Start Time is floored to the nearest lower multiple of Bar Length. For example, a scan time of -1500 ms with a Bar Length of 2000 ms floors to -2000 ms.

When a bar boundary is detected, `weaver~` enqueues a hit event into its internal queue containing:
- `rel_time`: The theoretical bar start time (for example: 0, 2000, 4000).
- Trigger Time: The actual signal ramp value at the exact moment the hit was captured.

### Step 3: Dictionary Lookup and Metadata Retrieval
On the main thread, `weaver~` takes `rel_time` and converts it into a string key (for example, "0", "2000", or "4000").

It then looks up the bar entry in the transcript dictionary under the corresponding track and bar key, extracting three key fields:
1. Palette Name: The name of the audio buffer containing the source audio (such as "1.wav").
2. Dict Offset: The palette anchor offset in milliseconds specified in the dictionary (such as 10000.0 ms).
3. Rating: A floating-point rating value used for dynamic volume scaling (such as 1.0 or 0.8).

These parameters are safely handed back to the DSP thread.

### Step 4: DSP Slot Toggling and Active Slot Offset Assignment
When the DSP thread receives the new metadata, it toggles between two crossfade slots (Slot 0 and Slot 1) to transition smoothly from the current audio slice to the new audio slice.

The offset stored for the new active slot is assigned directly from the dictionary metadata:

Slot Offset = Dict Offset

Where:
- Dict Offset is the offset value from `transcript.json` (in milliseconds).

### Step 5: Real-Time Source Sample Read Position

#### Fundamental Architectural Principles
The offsets stored in `transcript.json` mark the **start timestamp of the song when the palette was initially recorded**.

Because `Dict Offset` represents where the beginning of the song aligns within that specific palette recording, the palette read head aligns directly with the song playback timeline (`Current Time`).

Therefore, the exact read position in the palette buffer for any frame during song playback occurring at time `Current Time` (in milliseconds) is:

Read Position (ms) = Dict Offset + Current Time

In plain English: **`Dict Offset` anchors the start of the song within the palette recording. As song playback advances (`Current Time`), the read position moves forward sample-by-sample from that anchor point.**

#### Definitions of All Variables
To make the distinction crystal clear:
- **`Dict Offset`**: The absolute millisecond timestamp inside the source palette WAV file where song start (time 0 ms) was recorded (e.g. 10,000 ms). This comes directly from `transcript.json`.
- **`Current Time`**: The continuously advancing millisecond timestamp on the global song timeline for the specific sample frame currently being processed in the DSP loop (e.g. 4,000 ms, 4,001 ms, 4,500 ms).
- **`Slot Offset`**: The offset assigned to the track's active slot state (`tr->offset[slot]`), which equals `Dict Offset`.
- **`Read Position`**: The calculated millisecond position inside the source palette buffer for the sample frame currently being synthesized (`Read Position = Dict Offset + Current Time`).

#### Practical Numerical Example
Suppose we have a song with 2,000 ms bars (120 BPM):

1. **Bar Setup**:
   - Bar 0 covers song time 0 ms to 2,000 ms.
   - Bar 1 covers song time 2,000 ms to 4,000 ms.
   - Bar 2 starts at song time 4,000 ms.
2. **Transcript Entry**:
   - In `transcript.json`, Bar 2 is assigned to `palette_1.wav` with `Dict Offset = 10,000 ms`. This means `palette_1.wav` was recorded starting at 10,000 ms when the song began.
3. **Step 4 Slot Assignment**:
   - When Bar 2 is triggered, `weaver~` assigns:
     `Slot Offset = Dict Offset = 10,000 ms`
4. **Step 5 Frame Processing in DSP Loop**:
   - **Sample 1 (Exact Start of Bar 2)**:
     - `Current Time = 4,000 ms`
     - `Read Position = Dict Offset + Current Time`
     - `Read Position = 10,000 ms + 4,000 ms = 14,000 ms`
     - *Result*: `weaver~` reads sample 14,000 ms from `palette_1.wav` (the exact location where Bar 2 was recorded during that palette pass!).
   - **Sample 22,050 (25% through Bar 2 at 44.1kHz)**:
     - `Current Time = 4,500 ms`
     - `Read Position = Dict Offset + Current Time`
     - `Read Position = 10,000 ms + 4,500 ms = 14,500 ms`
     - *Result*: `weaver~` reads sample 14,500 ms from `palette_1.wav` (exactly 500 ms into Bar 2!).
   - **Sample 88,199 (End of Bar 2)**:
     - `Current Time = 5,999 ms`
     - `Read Position = Dict Offset + Current Time`
     - `Read Position = 10,000 ms + 5,999 ms = 15,999 ms`
     - *Result*: `weaver~` reads sample 15,999 ms from `palette_1.wav` (1,999 ms into Bar 2, right before Bar 3 starts).

---

### Step 6: Fractional Sample Index and Linear Interpolation
To convert Read Position (ms) into an actual sample frame index within the palette buffer:

Raw Sample Index = Read Position (ms) * (Source Sample Rate / 1000.0)

Since Raw Sample Index is rarely an exact integer, `weaver~` takes the floor integer index (Lower Index), the next integer index (Upper Index), and the fractional remainder.

It then performs linear interpolation between the audio sample at Lower Index and the audio sample at Upper Index. This guarantees smooth, sub-sample accurate audio playback without pitch glitches.

### Step 7: Dynamic Gain Scaling and Crossfade Envelope Processing
If dynamic gain is enabled, `weaver~` tracks bar ratings within a rolling window across the song length. If a bar has a negative rating, `weaver~` scales its gain between 0.0 and 1.0 based on how negative it is relative to the minimum rating seen in the rolling window.

At the same time, `weaver~` processes an adaptive crossfade envelope (using low and high millisecond limits) to fade out the previous slot while fading in the new slot.

### Step 8: Writing to Destination Buffer Memory
Finally, the interpolated samples from both crossfade slots are multiplied by their respective crossfade envelope levels and dynamic gain factors, summed together, and written into the destination polybuffer channel.

---

## 3. Detailed Analysis: Potential Failure Modes and Audio Playback Issues

Even when `transcript.json` contains valid and correct dictionary values, audio playback can sound wrong, click, or fail completely. Below is a detailed breakdown of the architectural and logical reasons why this happens.

### Issue 1: Thread Handover Latency and Skipped Transients
- **The Mechanism**: Hit detection occurs instantly on the audio DSP thread, but querying the dictionary happens on the main GUI thread.
- **The Root Cause**: The main thread can be delayed by UI redrawing, disk activity, or patcher operations. During a 20ms to 50ms main thread delay:
  1. The DSP thread continues playing audio from the previous bar while waiting for the dictionary response.
  2. When the main thread finally returns the new palette and offset data, 40ms of playback time has already elapsed in the current bar.
  3. When the DSP thread applies `Read Position (ms) = Dict Offset + Current Time`, `Current Time` is already 40ms past the bar start boundary.
- **The Result**: The read head immediately jumps to `Dict Offset + 40ms`, completely skipping the first 40ms of the new audio slice (where the drum kick or attack transient lives). This produces a dull, quiet click instead of a punchy drum hit.

### Issue 2: Dictionary Key Format Mismatch (Symbols vs Floats)
- **The Mechanism**: `weaver~` formats bar start timestamps into integer strings (such as "0", "2000", "4000").
- **The Root Cause**: If an external script or patcher formats keys in the transcript dictionary as floating-point strings (such as "2000.0") or numeric atoms rather than standard integer strings:
  1. The dictionary lookup fails to find a matching bar entry.
  2. `weaver~` falls back to its default missing-bar behavior, setting the palette symbol to "-".
- **The Result**: The track goes completely silent for that bar, even though valid data exists in the dictionary under a slightly different key format.

### Issue 3: Palette Buffer Naming and Prefix Discrepancies
- **The Mechanism**: Palette WAV files on disk often use prefixes (for example: `palette_1.wav`, `palette_2.wav`), whereas transcript files store stripped keys (such as "1.wav").
- **The Root Cause**: If `weaver~` looks for a buffer named "1.wav" but the polybuffer object loaded buffers named "palette_1" or "stems.1", the buffer lookup fails. `weaver~` then attempts to fall back to stem audio using raw linear playback.
- **The Result**: The object fails to load the intended palette slice and instead plays raw un-edited stem audio.

### Issue 4: Sample Rate Mismatch
- **The Mechanism**: Frame index calculation relies on the source buffer's reported sample rate.
- **The Root Cause**: If a source WAV file is recorded at 48000 Hz but the system audio driver or buffer object reports 44100 Hz (or an uninitialized rate of 0), the sample frame calculation will be off by roughly 9 percent.
- **The Result**: The audio slice plays back at the wrong pitch and speed, and reads from incorrect physical positions in the audio buffer.

### Issue 5: Unbounded Crossfade Busy Lock
- **The Mechanism**: `weaver~` checks whether a track is busy before allowing a new bar hit to trigger. A track remains "busy" while its crossfade ramps are still active.
- **The Root Cause**: If the high crossfade limit attribute (`high`) is set to a long duration (such as 4999 ms) or if signal amplitudes stay high, the crossfade ramp may take longer than a single bar to finish.
- **The Result**: When the next bar boundary arrives, `weaver~` sees that the track is still busy and ignores the new bar trigger entirely. The song stays stuck playing the previous bar's palette slice.

---

## 4. Parameter Summary Matrix

| Parameter | Source / Formula | Impact on Read Position |
| :--- | :--- | :--- |
| **Scan Time** | Signal Ramp + Most Negative Bar Offset | Global song timeline position in milliseconds |
| **Bar Start Time** | Floored multiple of Bar Length | Used to format dictionary bar keys (e.g., "2000") |
| **Dict Offset** | "offset" value from `transcript.json` | Start timestamp of the song when the palette was recorded |
| **Current Time** | Signal Ramp + Most Negative Bar Offset | Real-time playback position on global song timeline |
| **Read Position** | Dict Offset + Current Time | Exact millisecond position inside the palette WAV |
| **Raw Sample Index** | Read Position * (Source Sample Rate / 1000.0) | Floating-point sample index for interpolation |

---

## 5. Summary and Recommendations

To ensure rock-solid audio playback and eliminate slice calculation glitches in `weaver~`:
1. **Direct Palette Read Alignment**: `weaver~` reads palette audio at `Read Position = Dict Offset + Current Time`, mapping song playback directly into recorded palette space.
2. **Robust Dictionary Key Matching**: Ensure transcript generators format keys as plain integer strings (e.g., "2000"), and update lookup code to handle floating-point fallback keys gracefully.
3. **Guard Against Busy Locks**: Cap crossfade durations so they never exceed the duration of a bar, ensuring `weaver~` is always ready to trigger the next slice.

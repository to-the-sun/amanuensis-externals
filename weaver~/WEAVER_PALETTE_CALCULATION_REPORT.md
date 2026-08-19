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
| 10. Calculate Slot Read Offset: Calculated Offset = Dict Offset - Trigger Time    |
| 11. Compute Sample Index: Source Time (ms) = Calculated Offset + Current Time     |
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

### Step 4: DSP Slot Toggling and Calculated Read Offset
When the DSP thread receives the new metadata, it toggles between two crossfade slots (Slot 0 and Slot 1) to transition smoothly from the current audio slice to the new audio slice.

The key offset stored for the new active slot is calculated as:

Calculated Offset = Dict Offset - Trigger Time

Where:
- Dict Offset is the offset value from `transcript.json` (in milliseconds).
- Trigger Time is the scan time recorded when the bar boundary was triggered.

### Step 5: Real-Time Source Sample Read Position
For any frame during song playback occurring at absolute time Current Time (in milliseconds):

Source Time (ms) = Calculated Offset + Current Time

Substituting Calculated Offset = Dict Offset - Trigger Time gives the fundamental read position equation:

Read Position (ms) = Dict Offset + (Current Time - Trigger Time)

In plain English: **The audio position inside the palette buffer begins at Dict Offset when Current Time equals Trigger Time, and advances linearly sample-by-sample alongside song playback!**

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

### Issue 1: Sub-Vector Jitter Between Trigger Time and Theoretical Bar Start Time
- **The Mechanism**: In Step 4, Calculated Offset is computed as `Dict Offset - Trigger Time`, where Trigger Time is the actual ramp position when hit detection occurred. However, palette files are typically authored assuming that Dict Offset aligns perfectly with the theoretical bar start boundary (Bar Start Time).
- **The Root Cause**: If a bar boundary occurs between DSP vectors, Trigger Time might be captured slightly late (for example, at 2012.3 ms instead of exactly 2000.0 ms).
- **The Result**: As a consequence, `Read Position (ms)` reaches Dict Offset 12.3 milliseconds late. This causes a subtle rhythmic delay or transient smearing at the start of every bar.
- **The Fix**: Calculating offset using the theoretical Bar Start Time (`Calculated Offset = Dict Offset - Bar Start Time`) ensures exact alignment regardless of vector boundary timing.

### Issue 2: Thread Handover Latency and Skipped Transients
- **The Mechanism**: Hit detection occurs instantly on the audio DSP thread, but querying the dictionary happens on the main GUI thread.
- **The Root Cause**: The main thread can be delayed by UI redrawing, disk activity, or patcher operations. During a 20ms to 50ms main thread delay:
  1. The DSP thread continues playing audio from the previous bar while waiting for the dictionary response.
  2. When the main thread finally returns the new palette and offset data, 40ms of playback time has already elapsed in the current bar.
  3. When the DSP thread applies `Read Position (ms) = Dict Offset + (Current Time - Trigger Time)`, Current Time is already 40ms past Trigger Time.
- **The Result**: The read head immediately jumps to `Dict Offset + 40ms`, completely skipping the first 40ms of the new audio slice (where the drum kick or attack transient lives). This produces a dull, quiet click instead of a punchy drum hit.

### Issue 3: Dictionary Key Format Mismatch (Symbols vs Floats)
- **The Mechanism**: `weaver~` formats bar start timestamps into integer strings (such as "0", "2000", "4000").
- **The Root Cause**: If an external script or patcher formats keys in the transcript dictionary as floating-point strings (such as "2000.0") or numeric atoms rather than standard integer strings:
  1. The dictionary lookup fails to find a matching bar entry.
  2. `weaver~` falls back to its default missing-bar behavior, setting the palette symbol to "-".
- **The Result**: The track goes completely silent for that bar, even though valid data exists in the dictionary under a slightly different key format.

### Issue 4: Palette Buffer Naming and Prefix Discrepancies
- **The Mechanism**: Palette WAV files on disk often use prefixes (for example: `palette_1.wav`, `palette_2.wav`), whereas transcript files store stripped keys (such as "1.wav").
- **The Root Cause**: If `weaver~` looks for a buffer named "1.wav" but the polybuffer object loaded buffers named "palette_1" or "stems.1", the buffer lookup fails. `weaver~` then attempts to fall back to stem audio using raw linear playback.
- **The Result**: The object fails to load the intended palette slice and instead plays raw un-edited stem audio.

### Issue 5: Sample Rate Mismatch
- **The Mechanism**: Frame index calculation relies on the source buffer's reported sample rate.
- **The Root Cause**: If a source WAV file is recorded at 48000 Hz but the system audio driver or buffer object reports 44100 Hz (or an uninitialized rate of 0), the sample frame calculation will be off by roughly 9 percent.
- **The Result**: The audio slice plays back at the wrong pitch and speed, and reads from incorrect physical positions in the audio buffer.

### Issue 6: Unbounded Crossfade Busy Lock
- **The Mechanism**: `weaver~` checks whether a track is busy before allowing a new bar hit to trigger. A track remains "busy" while its crossfade ramps are still active.
- **The Root Cause**: If the high crossfade limit attribute (`high`) is set to a long duration (such as 4999 ms) or if signal amplitudes stay high, the crossfade ramp may take longer than a single bar to finish.
- **The Result**: When the next bar boundary arrives, `weaver~` sees that the track is still busy and ignores the new bar trigger entirely. The song stays stuck playing the previous bar's palette slice.

---

## 4. Parameter Summary Matrix

| Parameter | Source / Formula | Impact on Read Position |
| :--- | :--- | :--- |
| **Scan Time** | Signal Ramp + Most Negative Bar Offset | Global song timeline position in milliseconds |
| **Bar Start Time** | Floored multiple of Bar Length | Used to format dictionary bar keys (e.g., "2000") |
| **Trigger Time** | Ramp time captured at hit detection | Anchor point subtracted from Dict Offset |
| **Dict Offset** | "offset" value from `transcript.json` | Theoretical slice start point in the palette buffer |
| **Calculated Offset**| Dict Offset - Trigger Time | Constant offset applied during audio processing |
| **Read Position** | Dict Offset + (Current Time - Trigger Time) | Exact millisecond position inside the palette WAV |
| **Raw Sample Index** | Read Position * (Source Sample Rate / 1000.0) | Floating-point sample index for interpolation |

---

## 5. Summary and Recommendations

To ensure rock-solid audio playback and eliminate slice calculation glitches in `weaver~`:
1. **Anchor Offsets to Bar Boundaries**: Calculate slot offsets relative to theoretical Bar Start Time (`Dict Offset - Bar Start Time`) rather than captured Trigger Time (`Dict Offset - Trigger Time`).
2. **Robust Dictionary Key Matching**: Ensure transcript generators format keys as plain integer strings (e.g., "2000"), and update lookup code to handle floating-point fallback keys gracefully.
3. **Guard Against Busy Locks**: Cap crossfade durations so they never exceed the duration of a bar, ensuring `weaver~` is always ready to trigger the next slice.

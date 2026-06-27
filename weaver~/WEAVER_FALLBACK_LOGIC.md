# Weaver~ Palette Fallback Logic Report

This document details the internal mechanisms of the `weaver~` object regarding asset resolution and the specific conditions that trigger the `stems.[track_id]` fallback.

## 1. Overview
The `weaver~` object performs real-time audio "weaving" by crossfading between palette buffers based on a transcript dictionary. The resolution of these palettes follows a multi-stage logic path that transitions from the DSP (Audio) thread to the Main (Message) thread.

## 2. The Logic Pipeline

### Phase A: Bar Crossing Detection (DSP Thread)
1. The `weaver_process_vector` function monitors the incoming signal ramp.
2. For each track, it calculates a local scan position:
   `tr_scan = fmod(current_scan, tr->track_length)`
3. It compares the current `floor(tr_scan)` against the previous vector's value.
4. If a multiple of the `bar` buffer's value (e.g., 4000ms) has been crossed, a `TYPE_DATA` event is enqueued into the internal FIFO (`hit_bars`).
5. This event contains the `track_id` and the quantized `rel_time` (the bar's timestamp).

### Phase B: Task Scheduling (Main Thread)
1. The FIFO entry triggers the `audio_qelem`.
2. The Max scheduler executes `weaver_audio_qtask` on the Main thread.

### Phase C: Asset Resolution (Main Thread)
1. **Dictionary Lookup**: The task looks up the `track_id` (as a string) within the transcript dictionary.
2. **Bar Lookup**: Inside the track's sub-dictionary, it looks for the quantized bar timestamp (e.g., `"4000"`) as a key.
3. **Data Extraction**: If the bar key exists, it extracts the `palette` symbol and `offset` float.
4. **Existence Verification**: The object uses a temporary `buffer_ref` to verify the palette exists in the Max session. A "kick" (unbinding/rebinding) is performed if the initial lookup fails.
5. **Success Condition**: If the palette is found, the metadata is updated, and the DSP thread initiates a crossfade to that buffer at the specified offset.

## 3. The `stems.[track_id]` Fallback
The fallback to `stems` is triggered **only if** the bar was found in the dictionary, but the palette is deemed "non-existent." This happens if:
- The `palette` key is missing from the bar dictionary.
- The `palette` value is explicitly `"-"`.
- The named buffer (e.g., `palette_A`) is not loaded in the Max session.

**Fallback Actions:**
1. Construct a name: `stems.[track_id]` (e.g., `stems.1`).
2. Attempt to bind to this buffer (including a "kick" attempt).
3. If successful, use the `stems` buffer and set the playback `offset` to `0.0`.

## 4. The Fallback for the Fallback
If the `stems.[track_id]` buffer itself cannot be found in the session:
1. The object explicitly sets the palette to `"-"` (silence).
2. The offset is set to `0.0`.
3. This ensures the track fades to silence rather than hanging on previous audio or crashing.

## 5. Bypassing the Fallback
There are scenarios where the object defaults to `"-"` (silence) without ever attempting to look for a `stems` buffer:

- **Missing Bar Entry**: If the bar timestamp (e.g., `"4000"`) is completely absent from the track's dictionary, `found_in_dict` is set to `0`. The code immediately calls for silence.
- **Missing Dictionary**: If the entire transcript dictionary cannot be found, it defaults to silence.
- **Quantization Mismatch**: If the dictionary uses keys with different precision (e.g., `"4000.0"`) than the integer string the object generates (`"4000"`), the lookup fails, and the fallback is bypassed in favor of immediate silence.

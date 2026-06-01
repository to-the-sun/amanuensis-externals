# Technical Report: Audio Reconstruction and Gap-Filling in `weaver~`

## 1. Introduction: The "Crackly Audio" Problem
The reported "crackling" in the `weaver~` object was not a bug in the traditional sense, but rather a fundamental artifact of **temporal aliasing** and **discrete sampling mismatch**. When an audio engine uses a control signal (like a ramp) to drive the placement of audio into a buffer, it creates a "painter/canvas" relationship. If the painter (the DSP update) moves the brush (the ramp value) faster than the canvas (the buffer resolution) can record, the result is a series of disconnected dots rather than a continuous stroke. These dots are heard as high-frequency discontinuities—crackling.

## 2. Underlying Mechanisms: Time as a Signal
In `weaver~`, time is a first-class signal. The object receives a ramp representing the current position in the "Song." It then uses this position to:
1.  **Index Source Buffers:** Map `Song Time` + `Track Offset` to a position in a source buffer.
2.  **Index Destination Buffers:** Map `Song Time` directly to a position in the destination track buffer.

### The Problem with One-Sample-Per-Update
In the previous implementation, the object performed this mapping exactly once per output sample. 
*   **Aliasing:** By using `round()` to find source samples, it performed "point sampling." This is the audio equivalent of a low-resolution image; it creates "stair-step" artifacts that sound like metallic noise or aliasing.
*   **Gaps:** If the ramp moved by 2.5 samples in the time it took the output to move 1.0 sample, the intermediate 1.5 samples of the destination buffer were simply never touched. The buffer retained its previous values (or remained silent), creating holes.

## 3. Why This "Conclusion" is Necessary
The fix implemented—Gap-Filling and Linear Interpolation—is the mathematically necessary solution for any system where a **Variable-Rate Master** (the ramp) drives a **Fixed-Rate Slave** (the destination buffer).

### Gap-Filling (The Painter's Path)
To ensure continuity, the object must account for every destination sample that the ramp "passed over" since the last update. By iterating through the range `[last_f_dest + 1, f_curr]`, we are essentially "filling in the line" between the last two points the ramp touched. This ensures that even if the ramp is running at 10x speed, every single sample in the destination buffer is overwritten with the correct interpolated data.

### Linear Interpolation (Sub-Sample Accuracy)
By calculating the fractional position in the source buffer (`f_src_raw`) and blending the two nearest samples, we move from "point sampling" to "linear reconstruction." This drastically reduces the noise floor and eliminates the metallic aliasing characteristic of raw buffer indexing.

## 4. Is it a Band-Aid?
Calling this a "Band-Aid" suggests there is a cleaner way to avoid the problem entirely. In the context of digital signal processing, there are two alternatives, though both have significant trade-offs:

### Alternative A: Higher Internal Sample Rate (Oversampling)
We could run the internal logic of `weaver~` at 4x or 8x the host's sample rate. 
*   **Pros:** Reduces the probability of gaps; pushes aliasing frequencies above the range of human hearing.
*   **Cons:** Extremely CPU intensive; still doesn't *guarantee* continuity if the ramp moves extremely fast (e.g. a sudden jump).

### Alternative B: Delta-Based Processing
Instead of an absolute ramp, the object could receive a "delta" (number of samples to move).
*   **Pros:** Guarantees that every source sample is accounted for in order.
*   **Cons:** Breaks the "Time as a Signal" paradigm. If the delta-based system gets out of sync with the global clock, it has no way to "snap" back to the correct absolute position.

## 5. Addressing the Fundamental Issue
The "Fundamental Issue" is the **decoupling of the control signal from the audio clock**.

In a perfect system, the ramp would be generated with infinite precision and the audio engine would "pull" exactly the right amount of data to fill its current vector. However, in Max/MSP (and most modular environments), the ramp is just another signal. It is subject to the same quantization as the audio it controls.

### A More Fundamental Solution:
If we wanted to solve this at a lower level, we would need to move away from **Signal-Driven Time** and toward **Event-Driven Weaving**. 

In an Event-Driven model:
1.  The ramp generator wouldn't just send a value; it would send a **Manifest** of the range it just covered.
2.  `weaver~` would receive this manifest (e.g., "I just moved from 100.0ms to 105.0ms") and perform a high-speed "batch write" of that duration.

This is exactly what the `consolidate` message does! The "fix" we applied to the real-time `weaver_process_vector` is, in effect, bringing the robustness of the offline `consolidate` pipeline into the real-time DSP loop. We are treating every DSP update as a "mini-consolidation" of the gap since the last update.

## 6. Conclusion
The current implementation of gap-filling and linear interpolation is the standard "industry-best-practice" for high-quality variable-speed recording and playback (similar to how high-end samplers or varispeed tape emulations work). While it may feel like a "correction" of the input signal, it is actually the correct implementation of a **Reconstruction Filter** for the discrete time signal provided by the ramp. It transforms a sequence of discrete points into a continuous audio stream.

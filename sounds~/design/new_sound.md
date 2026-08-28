# Instructions for Designing a New Sound

When asked to "design a new sound by following the instructions in the `new_sound.md` file," follow this process to ensure the new sound is perceptually distinct and correctly integrated into the library. While designing a new sound, work entirely within the `sounds~/design/` directory. Everything you need can be found within it.

## Goal
The primary objective is to design a sound that is **as perceptually different as possible** from all existing sounds in the `sounds/` library. Focus on "human-perceptible" differences in timbre, dynamics, and texture, especially how these differences change, evolve and modulate over the course of a note.

## Step-by-Step Process

### 1. Survey the Existing Library
- Browse the `sounds/` directory.
- Review the `analysis.json` files in each subfolder.
- Pay attention to the `average_spectral_centroid` (brightness), `average_spectral_bandwidth`, and `mfcc_means` (timbral fingerprint).
- Look at the `temporal_data` arrays to understand how existing sounds evolve over time.

### 2. Formulate a Distinct Timbre
- First of all, you are allowed and encouraged to be creative.
- Choose a synthesis strategy that departs from existing ones (e.g., if existing sounds are mostly additive, try FM, subtractive with resonant filters, etc.).
- Aim for a different area of the frequency spectrum or a different temporal envelope.
- Sounds can be made distinct from each other by varying them over time. This can mean different ADSR envelopes, or modulating the timbre itself from one thing to another, all during a single note.
- Effects are not out of the question, as long as they can be implemented efficiently in C.
- Much dissonance is generally discouraged, but sounds can absolutely incorporate an atonal element, e.g. distortion, static. It is valid that this atonal element could be most or even all of the sound, as you might have when making drum samples, for example.
- We do not need to think of this in terms of only building "synths." If you want to try emulating acoustic instruments or even found sounds from nature, that would be interesting and acceptable. 

### 3. Implement the Design
- **Crucial:** Only modify the top-level `sound_design.c` file. **Never** modify the `sound_design.c` files stored inside existing `sounds/` subfolders.
- Update the `SOUND_DESIGN_VERSION` macro in `sound_design.h` to the next increment (find the highest numbered folder in `sounds/` and add 1).
- Implement your synthesis logic in the `render_midi` function within `sound_design.c`.
- The following "hook points" **must** be built into the sound design itself. Their values will be provided by standard MIDI, and they must do *something* in the code.
    1. **`note_on` and `note_off`:** A non-zero integer will arrive at `note_on` and a `0` will arrive at `note_off`. The typical use for these would be to signal the beginning of the attack and the beginning of the release, respectively, in a standard ADSR envelope. However, they could do anything else as well, for example, signal a modulation in pitch or some other aspect of timbre. Anything is fair game as long as they do something. It doesn't have to be related to amplitude at all.
    2. **`pitch`:** An integer `0` through `127` will arrive (typically coinciding with `note_on`) indicating the pitch the tonal portion of the sound must take on, in 12-tone equal temperament with MIDI note 69 (A4) at 440 Hz. To derive the hertz from the MIDI pitch, use:

        ```
        #include <math.h>
        double midi_to_hz_tuned(int midi_note, double a4_hz) {
            return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
        }
        ```

        Again, there may be an atonal portion to the sound as well, which this would not apply to. However, being atonal or non-tonal is different than simply being out-of-tune, so nothing microtonal.
    4. **`velocity`:** An integer `0` through `127` will arrive (typically coinciding with `note_on`) indicating the peak amplitude the sound must have at its loudest over the course of the note. The `velocity` must scale peak amplitude linearly from silence (`0`) to full volume (`127`).
- As a final, very general requirement, the sound must go from silence to audible at the point of `note_on` and must return to silence eventually at some point beyond `note_off`. 

### 4. Volume Calibration & Normalization
- All sounds must be normalized to a peak amplitude of **exactly 1.0 at velocity 127**.
- This normalization must be achieved by adjusting internal synthesis gain constants (e.g., scaling the final output in `render_note`) rather than using limiters or compressors.
- The standardized MIDI sequence uses a velocity of **127** for all notes.
- **Calibration Loop:**
    1. Compile with `make`.
    2. Run `./audio_engine`.
    3. Observe the `Peak amplitude` reported in the console.
    4. If the peak is not 1.0, calculate a correction factor: `new_gain = old_gain * (1.0 / current_peak)`.
    5. Update `sound_design.c` and repeat until the peak amplitude is exactly 1.0.

### 5. Standardized Analysis
- After calibration, running `./audio_engine` will:
    1. Render the standardized MIDI sequence (at velocity 127).
    2. Perform analysis every 50ms (RMS, Spectral Centroid, Bandwidth, Kurtosis, ZCR, MFCCs).
    3. Calculate the "Distance" from other sounds in the library.
    4. Save the `.wav`, the `sound_design.c` copy, and `analysis.json` into the new versioned subfolder.

### 6. Reciprocal Library Maintenance
- After generating a new sound, the older sounds' `analysis.json` files will not yet know their distance to this new sound.
- Run `./migrate_analysis` to re-analyze the entire library. This ensures every sound's `analysis.json` contains a complete `distances` dictionary reflecting its relationship to all other versions, including the one you just created.

### 7. Create a New `sounds~` Plugin 
- When finished with the above steps, the only things that need to be done outside of the `design/` folder are:
    1. Create a new `.dll` in the `modules/` folder for the new sound that can be run as expected from the `sounds~` object.
    2. Then recompile the `sounds~.mxe64` file itself to incorporate the sound into the object. 

## Technical Constraints & Format
- **Language:** C (C99 or later).
- **Dependencies:** `libsndfile`, `aubio`, `json-c`, `fftw3`.
  - On Debian/Ubuntu: `sudo apt-get install libsndfile1-dev libaubio-dev libjson-c-dev libfftw3-dev`
- **Temporal Analysis:** 50ms hop/window.
- **Data Format:** `temporal_data` must be a dictionary of arrays (e.g., `{"times": [...], "rms": [...]}`).
- **MIDI Consistency:** Always use the same MIDI sequence for all sounds to ensure a fair "timbre" comparison.
- **Distance Metric:** The system uses Euclidean distance on MFCC means as the primary "difference" score.
- **MIDI Handling:** Modules must be polyphonic and correctly handle sustained notes (e.g., by rendering any notes remaining in `active_notes` at the end of the `duration` without a release phase). This ensures compatibility with both listed sequences and potential live MIDI streams.
- **MIDI Pitch:** The tonal element of each sound must be able to take on the MIDI pitch being used (with a standard A4 = 440Hz tuning). Again, there may be an atonal portion to the sound as well, which this would not apply to. However, being atonal or non-tonal is different than simply being out-of-tune, so nothing microtonal.

## Subjective Judgment and Continued Iteration 
Analyze, compare, and iterate as many times as necessary on the new sound to achieve distinction. While the distance metric provides a quantitative guide, prioritize **human perception**. If two sounds have a high statistical distance but sound similar to a person, iterate further on the design to achieve true variety.

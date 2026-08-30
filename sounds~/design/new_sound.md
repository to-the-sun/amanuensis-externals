# Instructions for Designing a New Sound

When asked to "design a new sound by following the instructions in the `new_sound.md` file," follow this process to ensure the new sound is perceptually distinct and correctly integrated into the library. While designing a new sound, work entirely within the `sounds~/design/` directory. Everything you need can be found within it.

## Goal
The primary objective is to design a sound that is **as perceptually different as possible** from all existing sounds in the `sounds/` library. Focus on "human-perceptible" differences in timbre, dynamics, and texture, especially how these differences change, evolve and modulate over the course of a note.

## Step-by-Step Process

### 1. Survey the Existing Library
- Browse the `sounds/` directory.
- Review the `analysis.json` files in each subfolder.
- Note the `uniqueness_score` at the top of each `analysis.json` file.
- Inspect the 14 multi-probe diagnostic outputs under `probes`, which contain frame-by-frame 50ms `mfccs` vectors and `rms` energy values for active region segmentation.

### 2. Formulate a Distinct Timbre
- First of all, you are allowed and encouraged to be creative.
- Choose a synthesis strategy that departs from existing ones (e.g., if existing sounds are mostly additive, try FM, subtractive with resonant filters, etc.).
- Aim for a different area of the frequency spectrum or a different temporal envelope.
- Sounds can be made distinct from each other by varying them over time. This can mean different ADSR envelopes, or modulating the timbre itself from one thing to another, all during a single note.
- Effects are not out of the question, as long as they can be implemented efficiently in C.
- Much dissonance is generally discouraged, but sounds can absolutely incorporate an atonal element, e.g. distortion, static. It is valid that this atonal element could be most or even all of the sound, as you might have when making drum samples, for example.
- You do not need to think of this in terms of only building "synths." If you want to try emulating acoustic instruments or even found sounds from nature, that would be interesting and acceptable. 

### 3. Implement the Design
- **Crucial:** Only modify the top-level `sound_design.c` file. **Never** modify the `sound_design.c` files stored inside existing `sounds/` subfolders.
- Update the `SOUND_DESIGN_VERSION` macro in `sound_design.h` to the next increment (find the highest numbered folder in `sounds/` and add 1).
- Implement your synthesis logic in the `render_midi` function within `sound_design.c`.
- The sound design itself **must** accept the following arguments and utilize them somewhere in the code. Their values will be provided by standard MIDI. The basic requirements for these values are as follows, but a great way to make the sound unique is to get creative with including additional functions in the sound design for them. For example, `note_off` typically signals the beginning of the release in an ADSR envelope, however, it could do anything else as well or instead, such as cue some new modulation to begin. Or some other aspect of timbre could be based on the `velocity`, etc. Anything is fair game and you should not be limited by the following points, which are only the most basic requirements.
    1. **`note_on` and `note_off`:** A non-zero integer will arrive at `note_on` and a `0` will arrive at `note_off`. The only very general requirement for these is that the sound must go from silence to audible at the point of `note_on` and must return to silence eventually at some point beyond `note_off`.
    2. **`pitch`:** An integer `0` through `127` will arrive (typically coinciding with `note_on`) indicating the pitch the tonal portion of the sound must take on, in 12-tone equal temperament with MIDI note 69 (A4) at 440 Hz. To derive the hertz from the MIDI pitch, use:

        ```
        #include <math.h>
        double midi_to_hz_tuned(int midi_note, double a4_hz) {
            return a4_hz * pow(2.0, ((double)midi_note - 69.0) / 12.0);
        }
        ```

        Again, there may be an atonal portion to the sound as well, which this would not apply to. However, being atonal or non-tonal is different than simply being out-of-tune, so nothing microtonal.
    3. **`velocity`:** An integer `0` through `127` will arrive (typically coinciding with `note_on`) indicating the peak amplitude the sound must have at its loudest over the course of the note. The `velocity` must scale peak amplitude linearly from silence (`0`) to full volume (`127`).

### 4. Volume Calibration & Normalization
- All sounds must be normalized to a peak amplitude of **exactly 1.0 at velocity 127**.
- This normalization must be achieved by adjusting internal synthesis gain constants (e.g., scaling final output) rather than limiters or compressors.
- **Calibration Loop:**
    1. Compile with `make`.
    2. Run `./audio_engine`.
    3. Observe the `Peak amplitude` reported in the console for the `vel_127` probe.
    4. If the peak is not 1.0, calculate correction factor: `new_gain = old_gain * (1.0 / current_peak)`.
    5. Update `sound_design.c` and repeat until peak amplitude is exactly 1.0.

### 5. Standardized Analysis
- After calibration, running `./audio_engine` will:
    1. Render all 14 diagnostic probes across length, velocity, pitch, and articulation phrasing dimensions.
    2. Generate dedicated `staccato.wav` and `legato.wav` audio output files for the phrasing probes.
    3. Perform active region frame-by-frame 50ms MFCC analysis.
    4. Calculate composite multi-probe pairwise distances and the absolute nearest-neighbor `uniqueness_score`.
    5. Save `staccato.wav`, `legato.wav`, `sound_design.c` copy, `sound_design.h` copy, and `analysis.json` (with `uniqueness_score` at the very beginning) into the new versioned subfolder.

### 6. Reciprocal Library Maintenance
- After generating a new sound, older sounds' `analysis.json` files must be updated with pairwise distances to the new sound.
- Run `./migrate_analysis` to re-analyze all sound presets across the 14 diagnostic probes. This updates every preset's `analysis.json` with complete composite `distances`, `uniqueness_score`, and saved `staccato.wav` and `legato.wav` audio files.

### 7. Create a New `sounds~` Plugin 
- When finished with the above steps, move up to the parent `sounds~/` folder and:
    1. Create a new `.dll` in the `modules/` folder for the new sound.
    2. Recompile `sounds~.mxe64` to incorporate the new sound into the object.

## Technical Constraints & Format
- **Language:** C (C99 or later).
- **Dependencies:** `libsndfile`, `aubio`, `json-c`, `fftw3`.
  - On Debian/Ubuntu: `sudo apt-get install libsndfile1-dev libaubio-dev libjson-c-dev libfftw3-dev`
- **Temporal Analysis:** 50ms hop/window with active region frame-by-frame MFCC distance calculation.
- **Audio Output Files:** Phrasing probes output `staccato.wav` and `legato.wav`.
- **JSON Structure:** `analysis.json` puts `uniqueness_score` at the very beginning of the object, followed by `distances` and `probes` (containing 50ms frame `rms` and 13-band `mfccs` data for each probe).

## Subjective Judgment & Continued Iteration 
Analyze, compare, and iterate as many times as necessary on the new sound to achieve distinction. While the `uniqueness_score` provides a quantitative guide, prioritize **human perception**. If two sounds have a high statistical distance but sound similar to a person, iterate further on the design to achieve true variety.

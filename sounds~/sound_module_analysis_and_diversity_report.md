# Objective Analysis and Diversity Optimization for `sounds~` Synthesis Modules

## 1. Executive Summary

The `sounds~` object architecture relies on modular voice synthesis C engines declared via a stateful voice API (`create_voice`, `note_off_voice`, `process_voice`, `free_voice`, and `render_midi`). To encourage creative exploration and prevent redundant voice designs, the `design/` environment calculates pairwise distances between synthesis versions.

However, the legacy analysis system evaluates audio using a single 5-second, 4-note MIDI sequence analyzed with global 13-coefficient MFCC averages (`mfcc_means`). This approach fails to capture fundamental sound design characteristics:
1. Dynamic ADSR envelope profiles (attack sharpness, sustain levels, and release tails).
2. Non-linear velocity responses (timbral brightening, filter opening, or harmonic saturation).
3. Keyboard tracking across pitch registers.
4. Non-standard or unconventional behaviors triggered by `note_on` and `note_off` events.

This report outlines a comprehensive framework for objective audio analysis designed to measure and reward synthesis diversity. It addresses technical challenges such as silence-to-silence overestimation in long analytical buffers and introduces multi-probe diagnostic testing, phase-aligned ADSR feature extraction, and multi-dimensional diversity scoring metrics.

---

## 2. Limitations of Static Global Feature Analysis

The existing baseline in `analysis_utils.c` processes audio using a fixed hop size of 50 ms and collapses the resulting temporal frames into global mean values (`average_spectral_centroid`, `average_spectral_bandwidth`, `mfcc_means`). This static approach presents several key limitations:

### 2.1 Loss of Temporal Evolution
By averaging features across an entire audio buffer, transient attack dynamics are blended with steady-state sustain phases and decay tails. A sound with an aggressive 5 ms metallic transient followed by a dark sine pad produces a global MFCC mean similar to a soft, constant warm reed synth.

### 2.2 Blindness to Dynamic and Expressive Control
Standard MIDI synthesis relies on `velocity` and `pitch` modulation. The baseline sequence renders notes exclusively at maximum velocity (`velocity = 127`) across a narrow pitch range (MIDI notes 60, 64, 67, 72). Consequently, the analysis cannot determine whether a module implements expressive key tracking or velocity-dependent timbral shifting.

### 2.3 Unmodeled Event Rules
Sound modules are permitted to interpret `note_on` and `note_off` in unconventional ways (e.g., triggering secondary release bursts, initializing LFO pitch sweeps upon release, or maintaining infinite sustained drones). Static analysis treats these event-driven behaviors as homogeneous background audio.

---

## 3. Solving the Long-Buffer Silence Bias

Analyzing extended buffers (such as 10-second renders per note) is required to capture long release tails, ambient decays, and evolving reverberant structures. However, comparing fixed-length 10-second buffers directly introduces a major analytical flaw: **Silence-to-Silence Overestimation**.

### 3.1 The Silence Bias Mechanics
If two distinct sound modules both feature short release times (e.g., 200 ms release), a 10-second rendering window produces 0.2 seconds of active audio followed by 9.8 seconds of complete digital silence (`0.0`).

When calculating Euclidean or Manhattan distances across frame sequences or global averages, 98% of the data points compared between the two modules are identical zeros:

```
Sound A: [ Active Audio (0.2s) | Silence 0.0 ... 0.0 (9.8s) ]
Sound B: [ Active Audio (0.2s) | Silence 0.0 ... 0.0 (9.8s) ]
```

Because 9.8 seconds of zero-vectors match perfectly, the calculated distance between Sound A and Sound B drops drastically. The math falsely concludes that Sound A and Sound B are nearly identical, simply because both are silent for most of the window.

### 3.2 Mitigation Strategies

To evaluate long buffers objectively without distorting sound distance metrics, the analysis system must apply four structural techniques:

#### Strategy A: Active Region Segmentation and Silence Gating
Before extracting spectral features, the audio buffer is segmented into active audio regions using dual-threshold RMS energy gating:
* **Onset Threshold:** `-60 dBFS` to detect sound start.
* **Release Floor Threshold:** `-80 dBFS` (or local noise floor) to mark audio termination.

Analytical frame comparisons are restricted strictly to active frames (`T_start` to `T_end`). Frames beyond `T_end` are excluded from spectral and MFCC distance calculations.

#### Strategy B: Dynamic Time Warping (DTW) on Active Segments
Instead of comparing raw frame `i` of Sound A with frame `i` of Sound B on a rigid time grid, Dynamic Time Warping aligns the active spectral contours along an optimal non-linear path. DTW compares the *shape* of the sound's evolution during its actual audible duration, eliminating timing alignment mismatches caused by differing release durations.

#### Strategy C: Decoupled Duration and Energy Half-Life Features
Rather than allowing tail duration to dilute spectral features, the length of the sound is extracted as an explicit scalar feature vector:
* `effective_duration_seconds`: Total time audio remains above `-80 dBFS`.
* `energy_half_life_seconds`: Time required for cumulative RMS energy to reach 50% of its total integral.
* `t60_decay_time`: Time required for signal energy to decay by 60 dB following `note_off`.

These scalars are evaluated in a dedicated **Temporal Profile Distance Metric** separate from spectral timbral distance.

#### Strategy D: Normalized Phase-Aligned Stage Extraction
Each note buffer is split into four explicit physical phases relative to MIDI control signals:
1. **Attack Phase:** `note_on` timestamp to peak RMS frame.
2. **Decay/Sustain Phase:** Peak RMS frame to `note_off` timestamp.
3. **Early Release Phase:** `note_off` timestamp to `-20 dB` attenuation point.
4. **Late Release Phase:** `-20 dB` point to silence floor (`-80 dB`).

Features (MFCCs, Spectral Centroid, Flatness) are averaged *per phase*. Distance between modules is computed as a weighted sum of per-phase distances, ensuring that release tail silence cannot skew the attack or sustain comparisons.

---

## 4. Multi-Probe Diagnostic MIDI Test Suite

To evaluate synthesis modules across their full capabilities, the single-sequence test run in `audio_engine.c` should be replaced with a **Multi-Probe Test Suite**. Each probe tests a distinct operational dimension.

```
+-----------------------------------------------------------------------+
|                    Multi-Probe Test Suite Architecture                |
+-----------------------+-----------------------+-----------------------+
| Probe 1: ADSR Gate    | Probe 2: Velocity     | Probe 3: Pitch        |
| - Short Gate (100ms)  | - Low (v=16)          | - Bass (C1, pitch=24) |
| - Long Gate (4000ms)  | - Medium (v=64)       | - Mid  (C4, pitch=60) |
| - Tail Obs. (10000ms) | - High (v=127)        | - High (C7, pitch=96) |
+-----------------------+-----------------------+-----------------------+
| Probe 4: Expressive & Non-Standard Triggering                         |
| - Rapid Staccato (50ms interval re-triggers)                          |
| - Legato Overlap (note_on without prior note_off)                     |
| - Release Trigger Observation (post note_off output check)            |
+-----------------------------------------------------------------------+
```

### Probe 1: ADSR Gate and Envelope Dynamics Probe
* **Goal:** Quantify envelope phase lengths and release tail characteristics.
* **Execution:**
  * Render Note A: `note_on` at `t = 0.0s`, `note_off` at `t = 0.1s` (Short Gate / Staccato).
  * Render Note B: `note_on` at `t = 0.0s`, `note_off` at `t = 4.0s` (Long Gate / Sustained).
  * Capture audio for a total duration of 10.0 seconds per note.

### Probe 2: Velocity Dynamic Sweep Probe
* **Goal:** Test dynamic response, non-linear harmonic saturation, and gain scaling across velocity levels.
* **Execution:**
  * Render identical pitch (MIDI note 60) at velocities `v = 16`, `v = 48`, `v = 80`, `v = 112`, and `v = 127`.
  * Calculate peak amplitude response curve and spectral centroid shift vs velocity.

### Probe 3: Pitch Register and Key Tracking Probe
* **Goal:** Measure key tracking, filter scaling, and timbral uniformity/variation across registers.
* **Execution:**
  * Render notes across four octaves: MIDI notes 24 (C1), 48 (C3), 72 (C5), and 96 (C7).
  * Analyze octave-normalized spectral centroid ratios to determine if the sound brightens or darkens at extreme registers.

### Probe 4: Non-Standard Trigger & Modulation Probe
* **Goal:** Detect unconventional module behavior, such as release-triggered bursts, legato re-articulation, or note-off modulations.
* **Execution:**
  * Send overlapping `note_on` commands without intermediate `note_off` commands (Legato test).
  * Send rapid staccato bursts (10 ms gap between `note_off` and `note_on`).
  * Monitor signal energy after `note_off` to identify active release synthesis vs standard passive decay.

---

## 5. Objective Feature Representation Schema

To capture these characteristics, `analysis.json` should be expanded to include structured feature vectors representing the multi-probe results.

```json
{
  "version": 13,
  "envelope_profile": {
    "attack_time_ms": 12.5,
    "decay_time_ms": 180.0,
    "sustain_level_db": -6.2,
    "release_time_ms": 1450.0,
    "energy_half_life_ms": 320.0,
    "t60_decay_ms": 2100.0
  },
  "velocity_dynamics": {
    "amplitude_linearity_r2": 0.98,
    "centroid_velocity_slope": 14.2,
    "bandwidth_expansion_ratio": 1.45
  },
  "pitch_tracking": {
    "centroid_key_tracking_slope": 0.85,
    "spectral_flatness_register_std": 0.04
  },
  "non_standard_features": {
    "release_energy_ratio": 0.15,
    "legato_retrigger_transient_db": 3.2,
    "post_note_off_frequency_shift_hz": 120.0
  },
  "phase_mfccs": {
    "attack": [12.1, -3.2, 4.1, "..."],
    "sustain": [18.4, -1.1, 1.2, "..."],
    "release": [5.2, -8.4, 0.2, "..."]
  }
}
```

---

## 6. Mathematical Distance Metrics and Diversity Reward Framework

To encourage diversity when designing new sounds, the synthesis evaluation framework must reward modules that occupy under-represented regions of the feature space.

### 6.1 Multi-Feature Vector Weighted Distance
The distance `D(A, B)` between Module A and Module B is defined as a weighted sum of normalized sub-distances:

```
D(A, B) = w_spec * D_spec(A, B) + w_env * D_env(A, B) + w_vel * D_vel(A, B) + w_pitch * D_pitch(A, B)
```

Where:
* `D_spec(A, B)`: Euclidean distance between phase-aligned MFCC vectors (Attack, Sustain, Release phases).
* `D_env(A, B)`: Logarithmic distance between ADSR temporal parameters:

  `D_env = sqrt( sum( (log(param_A) - log(param_B))^2 ) )`
* `D_vel(A, B)`: Euclidean distance between velocity-response slopes (amplitude response and timbral brightening curves).
* `D_pitch(A, B)`: Distance between register key-tracking profiles.
* `w_spec`, `w_env`, `w_vel`, `w_pitch`: Weight factors normalized such that their sum equals 1.0 (e.g., `0.4`, `0.3`, `0.15`, `0.15`).

### 6.2 Mahalanobis Distance for Feature De-correlation
Certain acoustic features are inherently correlated (e.g., high RMS energy often correlates with higher spectral bandwidth). Using standard Euclidean distance over-counts correlated features.

Applying Mahalanobis distance normalizes feature scales and accounts for covariance across the sound library:

```
D_M(A, B) = sqrt( (v_A - v_B)^T * S^(-1) * (v_A - v_B) )
```

Where `v_A` and `v_B` are the feature vectors of Modules A and B, and `S` is the covariance matrix calculated across all existing sound modules in the library (`sounds/1/` through `sounds/N/`).

### 6.3 Diversity Reward Function (Rarity and Sparse-Cluster Scoring)
To score how much a new module expands the sound library's diversity, we define a **Diversity Score** `R(N)` for a candidate module `N`:

#### K-Nearest Neighbor (KNN) Density Penalty
Calculate the average distance from candidate module `N` to its `K` nearest neighbors in the library (e.g., `K = 3`):

```
Density_Distance(N) = (1 / K) * sum_{i=1..K} D(N, Neighbor_i)
```

#### Minimum Separation Constraint
A candidate module must exceed a minimum separation threshold `D_min` from all existing modules to ensure it is distinct:

```
Is_Unique = min_i( D(N, Module_i) ) > D_min
```

#### Objective Diversity Score Formula
The overall diversity reward combines sparse-cluster distance with novelty rewards for non-standard parameters:

```
Reward(N) = Density_Distance(N) * (1.0 + alpha * Rarity_Bonus(N))
```

Where `Rarity_Bonus(N)` measures how far module `N`'s ADSR parameters or velocity profiles deviate from the library mean, and `alpha` is a scaling factor (e.g., `0.25`).

---

## 7. Implementation Strategy for the `sounds~` Codebase

Integrating this objective analysis framework into the existing project requires updates across three main areas:

```
+-----------------------------------------------------------------------+
|                           System Integration                          |
+-----------------------------------------------------------------------+
| 1. analysis_utils.c / .h                                              |
|    - Implement multi-probe MIDI playback driver.                      |
|    - Add RMS energy gating and active region truncation.              |
|    - Implement phase-aligned ADSR feature extraction.                 |
+-----------------------------------------------------------------------+
| 2. audio_engine.c & migrate_analysis.c                                |
|    - Update analysis loop to iterate through multi-probe test suite.  |
|    - Compute Mahalanobis distance covariance matrix across sounds/.   |
|    - Store updated analysis profiles and distances in analysis.json.  |
+-----------------------------------------------------------------------+
| 3. new_sound.md Guidelines                                            |
|    - Document multi-probe criteria and diversity reward feedback.     |
|    - Guide sound designers on leveraging velocity curves and ADSR     |
|      shaping to maximize their module's diversity score.              |
+-----------------------------------------------------------------------+
```

### 7.1 Key Steps
1. **Extend `analysis_utils.c`**: Add functions for active region segmentation (`get_active_audio_bounds`), envelope phase extraction (`extract_adsr_profile`), and multi-probe test sequence rendering.
2. **Update `audio_engine.c`**: Replace `DEFAULT_MIDI_SEQUENCE` with the multi-probe suite. Compute multi-feature vectors and update `analysis.json`.
3. **Refine `migrate_analysis.c`**: Update library-wide re-indexing to compute covariance matrices and output updated pairwise distance tables and diversity scores.
4. **Update Design Guidelines (`new_sound.md`)**: Provide clear documentation on how sound design modules are evaluated, encouraging creators to experiment with envelope shapes, velocity modulations, and unique trigger rules.

---

## 8. Conclusion

By shifting from static global MFCC averages to active-region, multi-probe feature extraction, the `sounds~` analysis framework can objectively evaluate dynamic sound characteristics.

This approach addresses the silence bias inherent in long-buffer comparisons, models expressive MIDI controls, and provides a quantitative diversity metric. The resulting framework ensures that new sound modules are rewarded for introducing genuine sonic variety to the library.

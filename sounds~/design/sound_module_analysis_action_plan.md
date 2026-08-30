# Sound Module Analysis and Diversity Action Plan and Technical Report

## Overview and Objectives

This report outlines the architecture and action plan for evaluating sound module diversity in the `sounds~` synthesis engine. To ensure that timbral distance and diversity comparisons reflect actual acoustical content rather than statistical artifacts, global mean averaging across temporal frames is replaced with frame-by-frame Mel-Frequency Cepstral Coefficients (MFCC) analysis at uniform 50-millisecond intervals.

Furthermore, active region segmentation is implemented to avoid false similarity artifacts resulting from silent trailing frames. Finally, a multi-probe diagnostic test suite evaluates sound design modules across specific physical dimensions: note duration, velocity response, pitch registers, and articulation phrasing.

---

## Architectural Principles

### 1. Uniform 50-Millisecond MFCC Frame Analysis
- Audio buffers rendered for diagnostic probes are analyzed using 50 ms hop sizes and window lengths.
- Rather than collapsing resulting temporal frame vectors into a single global mean vector, the complete sequence of 13-coefficient MFCC vectors is retained.
- This uniform frame-by-frame representation avoids biasing or over-emphasizing specific temporal segments while capturing dynamic envelope changes, timbre shifts, and release decays across time.

### 2. Active Region Segmentation
When comparing temporal MFCC sequences between two sound modules:
- An active region for each sound module is determined by detecting frames where signal energy exceeds an RMS silence threshold (0.0001 linear RMS, equivalent to -80 dB relative to full scale).
- **Active vs. Active Frames**: Standard frame-by-frame Euclidean distance is computed between 13-band MFCC vectors.
- **Active vs. Silent (Zero) Frames**: When one sound module's active region extends longer than another's, the active frame MFCCs of the longer sound are compared against zero vectors for the shorter sound.
- **Silent vs. Silent Frames**: When both sound modules extend beyond their respective active regions into silence, zero vectors are NOT compared against zero vectors. No comparison is performed for these frame indices, preventing silent trailing tails from artificially inflating similarity or skewing distance metrics.

---

## Multi-Probe Diagnostic Test Suite Specification

The multi-probe test suite uses four distinct probe categories. When testing a specific parameter category, all unvaried parameters are fixed at designated standard baseline values:
- Standard Pitch: MIDI Note 60 (Middle C)
- Standard Velocity: 80 (Mezzo-forte)
- Standard Note Duration: 1000 milliseconds (1.0 second)

### Probe 1: Note Duration (Length Probe)
Evaluates sustained decay and release characteristics across four standard note-on to note-off durations:
1. 50 milliseconds (Ultra-short transient)
2. 250 milliseconds (Short staccato)
3. 1000 milliseconds (Standard sustained note)
4. 3000 milliseconds (Long sustained pad/decay)

### Probe 2: Dynamic Velocity (Velocity Probe)
Evaluates velocity response, timbral brightness scaling, and non-linear gain transfer across four standard MIDI velocity levels:
1. Velocity 16 (Pianissimo)
2. Velocity 48 (Piano/Mezzo-piano)
3. Velocity 80 (Mezzo-forte / Median baseline)
4. Velocity 127 (Fortissimo)

### Probe 3: Pitch Frequency (Pitch Probe)
Evaluates register balance, spectral energy distribution, and oscillator tuning across four standard MIDI pitch registers:
1. MIDI Note 24 (Sub-bass, ~32.7 Hz)
2. MIDI Note 48 (Bass/Low-mid, ~130.8 Hz)
3. MIDI Note 72 (Treble, ~523.2 Hz)
4. MIDI Note 96 (High treble, ~2093.0 Hz)

### Probe 4: Articulation Phrasing (Phrasing Probe)
Evaluates retriggering, overlap behavior, and voice allocation using two standardized phrase sequences with identical pitch and velocity variations across all sound modules:
1. **Staccato Phrase**: Rapid sequence of short notes (50ms - 100ms duration with short gaps) across a mix of pitches (e.g., MIDI 60, 64, 67, 72) and velocities.
2. **Legato Phrase**: Overlapping sustained notes (500ms - 1500ms duration) creating voice transitions and legato crossfades across the same standardized mix of pitches and velocities.

---

## Distance Metric Calculation

### Pairwise Distance Matrix Computation Across Multi-Probe Outputs
To determine the distance `D(A, B)` between two sound modules `A` and `B`, pairwise frame-by-frame MFCC distances are evaluated across each diagnostic probe `p` in the test suite:
- **Probe Pairwise Distance `D_p(A, B)`**: Calculated using Strategy A active region segmentation over the sequence of 13-band MFCC vectors for probe `p`.
- **Composite Distance `D(A, B)`**: Computed as the weighted sum of individual probe distances:
```
D(A, B) = sum_{p} w_p * D_p(A, B)
```
where probe weights `w_p` are normalized such that `sum_{p} w_p = 1.0` (with equal weighting `w_p = 1 / N_probes` by default).

---

### Diversity Reward Function Formulations and Comparison

To evaluate candidate sound modules for inclusion in the preset library, several reward function strategies were evaluated to quantify novelty, avoid redundant presets, and promote library diversity.

#### 1. K-Nearest Neighbor (KNN) Density Penalty with Minimum Separation Constraint
- **Formulation**:
  - `Density_Distance(N) = (1 / K) * sum_{i=1..K} D(N, Neighbor_i)`
  - `Is_Unique = min_i( D(N, Module_i) ) > D_min`
  - `Reward(N) = Is_Unique ? Density_Distance(N) : 0`
- **Pros**: Directly measures local cluster sparsity around candidate module `N`. The hard cutoff `D_min` guarantees no near-duplicate modules are added to the library. Computationally efficient for local neighborhood queries.
- **Cons**: The hard threshold creates a step-function discontinuity where a candidate module at `D_min - 0.001` receives zero score while `D_min + 0.001` receives full reward. Does not reward modules that expand the global boundaries or convex hull of the library space.

#### 2. Continuous Soft-Min Exponential Decay Reward
- **Formulation**:
  - `Uniqueness_Factor(N) = 1.0 - exp(- (min_i D(N, Module_i) / D_scale)^2)`
  - `Reward(N) = Uniqueness_Factor(N) * Density_Distance(N)`
- **Pros**: Provides a smooth, continuous penalty gradient from 0.0 (identical duplicate) to 1.0 (well isolated). Eliminates sharp boundary artifacts and behaves well in continuous optimization or evolutionary search algorithms.
- **Cons**: Requires tuning `D_scale`. In multi-objective evolutionary search, soft penalties can occasionally allow subtle duplicates to pass if secondary objectives (e.g. stability or CPU efficiency) score highly.

#### 3. Global Library Centroid and Variance Expansion
- **Formulation**:
  - `Library_Centroid C = (1 / M) * sum_{j=1..M} Module_j`
  - `Spread_Expansion(N) = D(N, C) + [ Variance(Library + {N}) - Variance(Library) ]`
- **Pros**: Explicitly rewards candidate modules that push the boundary of the sound library outward into unexplored timbral regions, maximizing overall library coverage.
- **Cons**: Vulnerable to extreme outliers or unmusical synthesis artifacts, which could receive disproportionately high scores merely by being far away from all existing presets.

---

### Selected Hybrid Diversity Reward Function
To balance strict duplicate prevention, local cluster sparsity, and global timbral expansion, a hybrid diversity reward function is adopted:

```
Is_Unique = min_i( D(N, Module_i) ) >= D_min

Density_Distance(N) = (1 / K) * sum_{i=1..K} D(N, Neighbor_i)   [K = 3]

Centroid_Distance(N) = D(N, Library_Centroid)

Reward(N) = Is_Unique ? [ w_knn * Density_Distance(N) + w_centroid * Centroid_Distance(N) ] : 0.0
```

- **Hard Uniqueness Guardrail**: Rejects any candidate module within `D_min` distance of an existing preset.
- **KNN Sparsity Incentive**: Prefers candidate modules situated in sparse regions rather than dense clusters.
- **Global Expansion Incentive**: Rewards candidate modules that expand the global timbral boundaries of the library.

---

## Technical Implementation Steps

1. **`analysis_utils.h` & `analysis_utils.c`**:
   - Define multi-probe MIDI sequence data structures for Length, Velocity (16, 48, 80, 127), Pitch, and Phrasing tests.
   - Update `analyze_audio()` to store 50ms temporal MFCC arrays and calculate active region frame limits.
   - Update `calculate_distance()` to implement Strategy A active region segmentation distance logic.

2. **`audio_engine.c` & `migrate_analysis.c`**:
   - Update rendering loops to execute each probe sequence.
   - Aggregate timbral distance metrics across all multi-probe diagnostic outputs and output detailed `analysis.json` files for each sound design module.

3. **Build & Verification**:
   - Recompile `audio_engine` and `migrate_analysis` binaries.
   - Run verification tests to confirm calculation accuracy and absence of silent zero-frame comparison artifacts.

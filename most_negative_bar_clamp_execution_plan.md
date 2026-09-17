# Execution Plan: Clamping `most_negative_bar` / `song_min` to a Maximum of 0.0

## Context & Rationale
In the current system, `most_negative_bar` (or `song_min` in `crucible`) represents the duration of negative lead-in / pickup bars before the primary transport `0.0 ms` mark.
Because song playback and transport ramps always begin at `0.0 ms`, if a transcript contains only positive bars (for instance, if the lowest bar in the transcript is at `+1000 ms`), the song still begins at `0.0 ms`.
Allowing `most_negative_bar` or `song_min` to evaluate to a positive number (such as `+1000 ms`) shifts the internal time origin forward, resulting in corrupted note timestamp calculations, playhead misalignment, incorrect stem read offsets, and visual grid distortion.

Therefore, `most_negative_bar` and `song_min` must always be clamped to a maximum of `0.0` (i.e. `val = (val < 0) ? val : 0`).

---

## Technical Changes Required

### 1. `crucible` (`crucible/crucible.c`)
- **`monitor_calculate_reaches()`**:
  Clamp calculated `song_min`:
  ```c
  if (song_min > 0) song_min = 0;
  ```
- **`crucible_recalculate_reaches()`**:
  Clamp calculated `song_min`:
  ```c
  if (song_min > 0) song_min = 0;
  ```

### 2. `buildspans` (`buildspans/buildspans.c`)
- **`buildspans_do_offset()`**:
  Sanitize incoming `most_negative_bar` parameter from inlet 2:
  ```c
  double clamped_most_neg = (most_negative_bar < 0.0) ? most_negative_bar : 0.0;
  x->most_negative_bar = clamped_most_neg;
  ```

### 3. `weaver~` (`weaver~/weaver~.c`)
- **`weaver_update_most_negative_bar()`**:
  Ensure `local_most_negative` is clamped to `<= 0.0` before comparing/assigning to `x->most_negative_bar`:
  ```c
  if (local_most_negative > 0.0) local_most_negative = 0.0;
  ```
- **`weaver_consolidate_worker()`**:
  Ensure `local_most_negative` is clamped to `<= 0.0`:
  ```c
  if (local_most_negative > 0.0) local_most_negative = 0.0;
  ```

### 4. `smartloop~` (`smartloop~/smartloop~.c`)
- **`smartloop_calculate()`**:
  Ensure `local_most_negative` is clamped to `<= 0.0`:
  ```c
  if (local_most_negative > 0.0) local_most_negative = 0.0;
  ```

### 5. `visualizer.py`
- **Pass 3 (Note Hash Marks Rendering)**:
  Clamp `most_negative_bar` to a maximum of `0.0`:
  ```python
  if most_negative_bar is None or most_negative_bar > 0.0:
      most_negative_bar = 0.0
  ```

---

## Build & Verification Strategy
1. Modify source files in C and Python.
2. Verify file updates using `read_file`.
3. Compile Windows 64-bit binaries using `x86_64-w64-mingw32-gcc` cross-compiler across `crucible`, `buildspans`, `weaver~`, and `smartloop~`.
4. Ensure all compiled `.mxe64` files are produced without compilation errors or warnings.

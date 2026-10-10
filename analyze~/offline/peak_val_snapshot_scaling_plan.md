# Cumulative Transience Snapshot Scaling by Peak Flux (peak_val) Implementation Plan

## Executive Summary
This document provides a detailed, step-by-step implementation guide for scaling transient snapshot slices by `peak_val` (the detected peak's spectral flux value) before accumulating them into the history buffer in the offline C cumulative transience algorithm (`analyze~/offline/cumulative_transience.c`).

---

## 1. Background & Rationale

### Current Behavior
In `analyzer_process_peak()` inside `analyze~/offline/cumulative_transience.c`:
1. When a transient peak is detected at index `p_idx`, its peak flux value `peak_val` is extracted:
   `peak_val = env_ptr[p_idx]`
2. The total score for the peak is computed as `peak_val` multiplied by the sum of qualifier scores:
   `total_score = peak_val * q_sum`
3. A rolling window snapshot slice (`peak_flux`) is populated with raw, unscaled spectral flux values directly from the envelope:
   `peak_flux[i] = env_ptr[start + i]`
4. This unscaled slice is added directly to the accumulated transience buffer `acc_buf` and saved into the active `SnapshotEntry` entry (`entry->snapshot`).

### Problem
Because snapshot slices added to `acc_buf` are unscaled, quiet transients (transients with low peak flux) contribute as much relative spectral shape energy to the cumulative history buffer as loud/intense transients.

### Proposed Solution
By scaling the entire snapshot slice by `peak_val` before adding it to `acc_buf`:
* Quiet transients (`peak_val` much less than 1.0) contribute significantly less energy to the accumulated history buffer contour.
* Prominent transients (`peak_val` high) dominate the accumulated transience contour, yielding stronger qualifier alignment for major rhythmic events.
* At the peak frame index itself, the accumulated flux scales quadratically (`peak_val * peak_val`), reinforcing true primary transient locations.

---

## 2. Targeted Files & Modifications

### File 1: `analyze~/offline/cumulative_transience.c`

#### Function: `analyzer_process_peak()`

**Current Implementation (Lines ~262-269):**
```c
    int win_len = self->window_ms + 1;
    int start = p_idx - self->window_ms;
    double peak_flux[BUFFER_LEN];
    for (int i = 0; i < win_len; i++) {
        int idx = start + i;
        peak_flux[i] = (idx < 0 || idx >= env_len) ? 0.0 : (double)env_ptr[idx];
    }
```

**Modified Implementation:**
```c
    int win_len = self->window_ms + 1;
    int start = p_idx - self->window_ms;
    double peak_val = (double)env_ptr[p_idx];
    double peak_flux[BUFFER_LEN];
    for (int i = 0; i < win_len; i++) {
        int idx = start + i;
        double raw_flux = (idx < 0 || idx >= env_len) ? 0.0 : (double)env_ptr[idx];
        peak_flux[i] = raw_flux * peak_val;
    }
```

#### Snapshot Subtraction & Cleanup Analysis
* In `analyzer_process_peak()`:
  `for (int i = 0; i < win_len; i++) acc_buf[i] += peak_flux[i];`
  `memcpy(entry->snapshot, peak_flux, sizeof(double) * win_len);`
* In `analyzer_cleanup_snapshots()`:
  `for (int j = 0; j < win_len; j++) acc_buf[j] -= e->snapshot[j];`
* Because `entry->snapshot` stores the exact scaled `peak_flux` array, when a snapshot ages out of the rolling window, subtracting `e->snapshot[j]` perfectly offsets the addition to `acc_buf`. No changes are required in `analyzer_cleanup_snapshots()` or `analyzer_destroy()`.

---

## 3. Build & Recompilation Instructions

When this change is implemented, both Linux and Windows 64-bit binaries must be recompiled.

### 1. Compile Linux Executable
In directory `analyze~/offline/`:
```bash
make clean && make
```
This builds `ct_analyze_headless`.

### 2. Cross-Compile Windows 64-bit Executable
Cross-compile the Windows binary using MinGW:
```bash
x86_64-w64-mingw32-gcc -O3 -I. -Wall -Wextra \
  ct_analyze_headless.c ct_exporter.c cumulative_transience.c \
  -o ct_analyze_headless.exe -lm -lpthread
```

---

## 4. Verification Steps

1. **Unit & Pipeline Execution Verification**:
   Run `ct_analyze_headless` on a test WAV file to confirm execution completes without memory errors or buffer overflows.
2. **Binary Generation Check**:
   Confirm both `ct_analyze_headless` and `ct_analyze_headless.exe` are built and trackable in git.
3. **Report Generation Inspection**:
   Generate static HTML analysis reports (`<audio_stem>_pattern_analysis.html`) and check that accumulated history buffer curves reflect scaled transient snapshot accumulation without visual corruption.

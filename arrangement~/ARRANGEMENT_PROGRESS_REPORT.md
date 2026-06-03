# Arrangement~ Development Progress Report

This report outlines the development of the `arrangement~` custom Max MSP external object, covering the transition from its Genexpr source to a robust, Multi-Channel (MC) enabled C implementation.

## Project Goal
The primary objective was to port a specific arrangement logic written in Genexpr into a high-performance C external. The object manages audio stems and palettes, performs time-ramp-based playback, handles quantized looping, and integrates dynamic crossfading for both auditioning and loop transitions.

## Development Milestones & Issue Resolution

### 1. Initial Porting (Genexpr to C)
- **Action**: Faithful sample-by-sample translation of the original logic.
- **Components**:
    - 7 Buffer references (`stem`, `palette`, `product`, `comp`, `stats`, `following`, `bar`).
    - 11 Parameters and 13 History variables maintained in the object struct.
    - Integration of the `shared/crossfade` module.
    - 7 Signal outlets mapping to `out1` through `out7`.

### 2. Instantiation Stability
- **Issue**: Max crashed immediately upon object instantiation.
- **Cause**: The use of standard Max symbols (like `_sym_nothing`) in `buffer_ref_new` requires the common symbol table to be initialized.
- **Solution**: Added `common_symbols_init()` to the `ext_main` entry point.

### 3. Multi-Channel (MC) Integration
- **Action**: Enhanced the object to support the `@chans` attribute.
- **Implementation**:
    - Refactored the state into a `t_arrangement_chan` struct.
    - Implemented dynamic allocation of channel instances based on `@chans`.
    - Set the `Z_MC_INLETS` flag to allow the object to handle MC signals.
    - Added the `setvalue` message (`setvalue [chan] [selector] [value]`) for granular control over individual instances.

### 4. Audio-Start Crash Protection
- **Issue**: Max crashed as soon as the audio engine was turned on.
- **Cause**: Concurrency issues and buffer/state access during instantiation or parameter changes. Specifically, the audio thread could attempt to access `chan_states` while it was being reallocated by the main thread.
- **Solutions**:
    - **Critical Sections**: Protected state modification and access using `t_critical` locks (`critical_enter` / `critical_exit`).
    - **Perform Routine snapshots**: In `arrangement_perform64`, used `critical_tryenter` to avoid blocking the audio thread and gracefully skip processing (zeroing outputs) if the lock is held by a state-changing operation.
    - **Channel Count Snapshot**: Captured `chans_dsp` during the DSP setup to ensure consistent indexing within the audio vector even if `@chans` is updated.

### 5. Multi-Channel Signal Mapping
- **Issue**: Signals were improperly mapped or interleaved in early MC attempts.
- **Solution**:
    - Implemented `setnumchannels` in `arrangement_dsp64` to inform the DSP chain of the requested width for all 7 outlets.
    - Corrected indexing in `arrangement_perform64` to match the Max MC signal format: `outs[outlet_idx * chans + chan_idx]`.
    - Implemented wrap-around logic for input signals (`ins[chan_idx % nchans_in]`) to ensure all instances receive a control ramp even if the input signal has fewer channels than the object instances.

## Current Status
The `arrangement~` object is now fully functional and stable. It supports:
- **Dynamic Configuration**: Change any buffer, parameter, or history via standard messages or `setvalue`.
- **High Performance**: Native C implementation with vector-based processing and efficient memory management.
- **Multi-Channel Native**: Built from the ground up to respect Max's MC ecosystem.
- **Robustness**: Hardened against instantiation and runtime crashes.

The folder contains:
- `arrangement~.c`: C source code.
- `Makefile`: Build instructions for x64 Windows (mingw).
- `arrangement~.mxe64`: Compiled binary.
- `arrangement~.maxref.xml`: Complete documentation.
- `ARRANGEMENT_PROGRESS_REPORT.md`: This report.

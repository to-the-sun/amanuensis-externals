# Weaver~ Standalone Unit Testing and Mock Framework

This directory (`weaver~/test_include/`) contains a native standalone C unit-testing, mock, and platform adapter framework for the `weaver~` Max MSP object. **Do not** run this test suite unless explicitly asked to by the user. It allows you to compile, run, and diagnose the core `weaver~` C object on a standard Linux environment (without needing Windows, macOS, or a running instance of Max MSP).

---

## Architecture Overview

The testing framework consists of three main parts:

1. **Max MSP Mock Layer (`max_mock.h`, `max_mock.c`)**:
   - Mocks the core Max MSP SDK structures: `t_symbol`, `t_atom`, `t_dictionary`, `t_atomarray`, `t_hashtab`, `t_pxobject`, `t_qelem`, `t_critical`, etc.
   - Provides thread-safe mock implementations of critical sections and thread structures using **POSIX threads** (`pthread_mutex_t`, `pthread_t`, `pthread_cond_t`).
   - Implements key-value lookups, symbol tables, and registered global dictionaries matching the Max SDK behavior.

2. **Platform & Network Adapter (`windows.h`, `winsock2.h`)**:
   - Maps Windows-specific socket calls (Winsock, `closesocket()`, `WSAGetLastError()`) used by `shared/visualize.c` directly to standard **UNIX POSIX socket APIs** (`close()`, `errno`, `fcntl()`, etc.).
   - Utilizes a mock `sockaddr_in` struct layout matching Winsock structure definitions, resolving Winsock-specific union accesses (such as `.sin_addr.S_un.S_addr`) gracefully on Linux without modifying the original shared source code.
   - Enables visualization data to be transmitted over TCP Port 8999 to `debug_visualizer.py` even on Linux!

3. **Test Harness (`weaver_test.c`)**:
   - Contains a built-in recursive-descent **JSON Parser** that parses a standard transcript file (e.g. `sampletranscript.json`) into nested mock `t_dictionary` tree structures.
   - Implements a fallback **programmatic transcript builder** to synthesize a dictionary on the fly.
   - Configures, registers, and populates mock buffers: `"bar"`, palette wav buffers (`"palette_A.wav"`, `"palette_B.wav"`), fallback buffers (`"stems.1"`, `"stems.2"`), and destination buffers (`"poly.1"`, `"poly.2"`).
   - Instantiates `weaver~` and simulates a continuous **ramp input signal** from `0.00` ms to the absolute song length, triggering DSP processing vectors (in chunks of 512 samples) and main-thread qelem queue tasks sequentially.
   - Intercepts and redirects all verbose logging outlet messages, posts, warnings, and errors to `weaver_verbose_log.txt`.

---

## How to Compile & Run

To build and run the test harness, execute the following commands in your shell:

### 1. Build the Test Runner
```bash
cd weaver~/test_include
make test_runner
```

### 2. Run with default sample transcript JSON
```bash
make run
```
*This command runs `./test_runner ../../sampletranscript.json`.*

### 3. Run with programmatic synthesized transcript
```bash
./test_runner
```

### 4. Clean up compilation and log artifacts
```bash
make clean
```

---

## Diagnostic Outputs

After running the test, the runner produces:

1. **`weaver_verbose_log.txt`**:
   - Captures every log emitted from the verbose outlet (Outlet 3), `object_warn`, `object_error`, and `object_post`.
   - Records details such as buffer attachments, fallback events, loop crossings, busy/fade statuses, and visualization socket logs.

2. **Console Output**:
   - Prints high-level progress percentages, simulation milestones, and a final **Destination Buffer Diagnostic** indicating whether written audio was successfully woven into `poly.1` and `poly.2` buffers.

3. **Active Visualization**:
   - If `@visualize` is enabled (`1` by default) and `debug_visualizer.py` is running on the host machine, the test runner will automatically connect to TCP Port 8999 and transmit high-frequency visualization coordinates, triggers, and gains, allowing visual diagnosis.

---

## Guide to Future Diagnoses

When debugging new issues or features with `weaver~`:

1. **Simulate Specific Scenarios**:
   - You can edit `synthesize_transcript()` in `weaver_test.c` to add custom bar ratings, missing bars, specific offset values, or custom palettes to test negative-territory boundaries, crossfade oscillations, or dynamic gains.
2. **Inspect Variable State**:
   - Since the test runner compiles directly as a standard C executable, you can add standard `printf()` statements or run standard profiling tools directly on the binary.
3. **Verify Regression**:
   - Run the unit test after making changes to `weaver~.c` to ensure there are no segfaults, memory leaks, or initialization errors.

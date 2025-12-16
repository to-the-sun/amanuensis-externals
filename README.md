# Max/MSP External Objects Project

This repository contains custom external objects for the Max/MSP programming language, written in C.

## Project Structure

The repository is organized into several directories:

-   `assemblespans/`, `createproject/`, `stemversion/`, `whichoffset/`: Each of these directories contains the source code for a single Max external object.
-   `shared/`: Contains common C code modules that can be shared across multiple external objects.
-   `max-sdk/`: Contains the Max SDK, which is required for building the external objects.
-   `gui.py`: A Python-based GUI for visualizing data from the objects.

Each external object's directory typically contains:

-   `<object-name>.c`: The C source code for the object.
-   `<object-name>.maxref.xml`: The XML documentation file for the object's inlets, outlets, and methods.
-   `<object-name>.mxe64`: The compiled 64-bit Windows binary for the object.
-   `Makefile`: The build script for compiling the object.

## Build Process

### Dependencies

1.  **Max SDK**: The project requires the Cycling '74 Max SDK. It must be cloned into the `max-sdk` directory at the repository root.
    ```bash
    git clone https://github.com/Cycling74/max-sdk.git --recursive
    ```
2.  **MinGW-w64**: The `x86_64-w64-mingw32-gcc` compiler is used for building the 64-bit Windows binaries (`.mxe64`).
    ```bash
    sudo apt-get update && sudo apt-get install -y mingw-w64
    ```

### Compilation

To build an individual external object, navigate into its specific directory and run the `make` command:

```bash
cd assemblespans/
make
```

## The `assemblespans` Object

The `assemblespans` object is designed to process musical note data and group it into "spans."

### Core Concepts

-   **Span**: A span is a contiguous sequence of musical "bars." A bar is a time interval defined by the `bar_length` attribute. A span is considered contiguous if the time gap between consecutive bars is no greater than `bar_length`.
-   **Working Memory**: The object's primary internal data structure is a `t_dictionary` called `working_memory`.
    -   The top-level keys of this dictionary are track numbers (e.g., "1", "2", "3").
    -   Each top-level key maps to a track-specific sub-dictionary.
-   **Track Dictionary**: Each track's sub-dictionary contains keys for each bar's timestamp (e.g., "44440", "49995").
-   **Bar Dictionary**: Each bar's sub-dictionary contains the actual note data:
    -   `absolutes`: A `t_atomarray` of the absolute millisecond timestamps of the notes in that bar.
    -   `scores`: A parallel `t_atomarray` of the scores for each note.
    -   `mean`: The calculated mean of the scores.
    -   `offset`: A *copy* of the global offset at the time the bar was created.
    -   `palette`: A *copy* of the global palette symbol at the time the bar was created.
    -   `span`: A *shared reference* to a `t_atomarray` that contains the timestamps of all bars in the current span for that track. This is back-propagated to all bars in the span whenever a new bar is added.

### Global vs. Per-Bar Properties

It is critical to distinguish between global properties of the object and the per-bar data that is stored.

-   **Global Properties**: These are stored as members of the main `_assemblespans` C struct and persist until they are changed by the user. They are not affected when a span is flushed.
    -   `current_track`: The currently active track number.
    -   `current_offset`: The global offset value.
    -   `bar_length`: The length of a bar in milliseconds.
    -   `current_palette`: The currently active palette symbol.
-   **Per-Bar Properties**: When a new bar is created, the current values of the global `current_offset` and `current_palette` are copied into that bar's dictionary. This provides a snapshot of the global state at the time of the bar's creation.

### Flushing Logic

A span is "flushed" (i.e., ended and output) under two conditions:

1.  **Manual Flush**: Receiving a `bang` in the first inlet triggers a flush of all active spans for all tracks.
2.  **Automatic Flush**: If a new timestamp-score pair is received that would create a bar that is not contiguous with the existing span (i.e., `new_bar_timestamp > last_bar_timestamp + bar_length`), the object automatically flushes the existing span for that track *before* creating the new bar, which then starts a new span.

The `assemblespans_flush_track` function handles the flushing process by:
1.  Sending the track number and the list of bar timestamps in the span to the outlets.
2.  Freeing the *entire* track dictionary object. This is a crucial step that recursively and safely frees all the bar sub-dictionaries and their contents.
3.  Deleting the top-level entry for that track from the `working_memory` dictionary.

## Development Workflow

When making changes to an object's C code:

1.  **Modify the C source file** (`<object-name>.c`).
2.  **Update documentation and assist messages**:
    -   Update the corresponding `.maxref.xml` file to reflect any changes to the object's functionality.
    -   Update the in-code `assist` messages to keep the inlet/outlet help text accurate.
3.  **Recompile the object** by running `make` in the object's directory to produce a new `.mxe64` binary.
4.  **Submit all modified files** for review, including the `.c`, `.maxref.xml`, and the newly compiled `.mxe64` files.

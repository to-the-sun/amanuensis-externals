# Max/MSP External Objects Project

This repository contains custom external objects for the Max/MSP programming language, written in C.

## Project Structure

The repository is organized into several directories:

-   `buildspans/`, `createproject/`, `stemversion/`, `whichoffset/`: Each of these directories contains the source code for a single Max external object.
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
2.  **MinGW-w64**: The `x86_64-w64-mingw32-gcc` compiler is used for building the 64-bit Windows binaries (`.mxe64`). The recommended way to install it on Windows is via **MSYS2**.

    **Installation Steps:**
    1.  **Download and install MSYS2** from [https://www.msys2.org/](https://www.msys2.org/). Follow the instructions on the site.
    2.  **Open the MSYS2 terminal** after installation and run the following command to install the toolchain:
        ```
        pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
        ```
        - Press `Enter` to accept the default package selection.
        - Type `Y` and press `Enter` to proceed with the installation.
    3.  **Add the MSYS2 tools to your Windows PATH.** This is a critical step to ensure both the compiler (`gcc`) and the `make` command can be found in your Command Prompt.
        - Search for "Edit the system environment variables" in the Windows search bar and open it.
        - Click "Environment Variables..."
        - In the "User variables" section, select the `Path` variable and click "Edit..."
        - You will need to add **two** new entries. Click "New" and add each of the following paths:
          - `C:\msys64\ucrt64\bin`  *(this is for the compiler, gcc.exe)*
          - `C:\msys64\usr\bin`    *(this is for build tools like make.exe)*
        - Click `OK` on all windows to save the changes.
    4.  **Verify the installation.** Open a **new** Command Prompt and run `gcc --version`. If it's installed correctly, you will see the compiler version information.

### Compilation

To build an individual external object, navigate into its specific directory and run the `make` command from a standard Windows Command Prompt or PowerShell:

```bash
cd buildspans/
make
```

## The `buildspans` Object

The `buildspans` object is designed to process musical note data and group it into "spans."

### Core Concepts

-   **Span**: A span is a contiguous sequence of musical "bars." A bar is a time interval defined by the `bar_length` attribute. A span is considered contiguous if the time gap between consecutive bars is no greater than `bar_length`.
-   **Working Memory**: The object's primary internal data structure is a `t_dictionary` called `building`.
    -   The keys of this dictionary are hierarchical: `palette::track-rounded_offset::bar_timestamp::property`.
-   **Bar Dictionary**: Each bar's data includes:
    -   `absolutes`: A `t_atomarray` of the absolute millisecond timestamps of the notes in that bar.
    -   `scores`: A parallel `t_atomarray` of the scores for each note.
    -   `mean`: The calculated mean of the scores.
    -   `offset`: A *copy* of the global offset at the time the bar was created.
    -   `palette`: A *copy* of the global palette symbol at the time the bar was created.
    -   `span`: A `t_atomarray` that contains the timestamps of all bars in the current span for that track. This is back-propagated to all bars in the span whenever a new bar is added.

### Global vs. Per-Bar Properties

It is critical to distinguish between global properties of the object and the per-bar data that is stored.

-   **Global Properties**: These are stored as members of the main `_buildspans` C struct and persist until they are changed by the user.
    -   `current_track`: The currently active track number.
    *   `current_offset`: The global offset value.
    *   `current_palette`: The currently active palette symbol.
    *   `local_bar_length`: The length of a bar in milliseconds.
-   **Automatic Offset Initialization**: If the offset has not yet been set (since instantiation or the last `clear` message) and a note list is received in Inlet 1, the first timestamp in that list is used to automatically initialize the global offset.
-   **Persistence after Flush**: When a span is flushed via a `bang`, **only** the `local_bar_length` is reset to 0. The `current_track`, `current_offset`, and `current_palette` persist at their last received values.

### Flushing Logic

A span is "flushed" (i.e., ended and output) under several conditions:

1.  **Manual Flush**: Receiving a `bang` in the first inlet triggers a flush of all active spans for all palettes.
2.  **Discontinuity**: If a new timestamp is received that would create a bar that is not contiguous with the existing span, the object automatically flushes the existing span for that track.
3.  **Rating-Based End**: A deferred rating check occurs when a new bar starts. If including the previous bar would have decreased the overall span rating, the span is ended before that bar.

The flushing process handles:
1.  Outputting the span data, track number, and detailed bar properties to the outlets.
2.  Deleting the entries for that track from the `building` dictionary.

## Development Workflow

When making changes to an object's C code:

1.  **Modify the C source file** (`<object-name>.c`).
2.  **Update documentation and assist messages**:
    -   Update the corresponding `.maxref.xml` file to reflect any changes to the object's functionality.
    -   Update the in-code `assist` messages to keep the inlet/outlet help text accurate.
3.  **Recompile the object** by running `make` in the object's directory to produce a new `.mxe64` binary.
4.  **Submit all modified files** for review, including the `.c`, `.maxref.xml`, and the newly compiled `.mxe64` files.

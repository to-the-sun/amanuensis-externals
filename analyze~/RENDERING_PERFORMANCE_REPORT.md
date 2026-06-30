# Video Rendering Performance Report

This report analyzes the slow video rendering performance in `analyze_files.py` and proposes optimization strategies.

## 1. Problem Identification: High Frame Latency

While the incremental analysis is now highly efficient, the video rendering stage remains a major bottleneck. The primary cause is the overhead of **Dynamic Artist Management** within Matplotlib's animation loop.

## 2. Technical Causes

### A. Lack of Blitting (`blit=False`)
The current animation is initialized with `blit=False`. This means that for every single frame (30 times per second), Matplotlib re-draws the **entire** figure, including the complex background grids, axes, and labels of all three subplots.

### B. Heavy Fragmented UI Logic
The `update(frame)` function contains significant logic that executes on every frame:
-   Filtering and searching through `all_processed_peaks`.
-   Iterating through `active_flashes`, `active_scores`, and `active_qualifiers`.
-   Calculating dynamic positions and alphas for multiple text and line artists.

### C. Expensive `fill_between` Operations
The historical 5s buffer visualization uses `fill_between` to create "flash" effects. These operations are computationally expensive because they involve creating and rasterizing complex polygons. When multiple flashes overlap, the rendering time increases non-linearly.

### D. Constant Artist Creation and Removal
The script frequently calls `.remove()` and creates new artists (like `ax_buf.axvline` or `ax_snapshot.text`) during every frame that contains a peak. This causes significant churn in Matplotlib's internal tree structure.

## 3. Proposed Solutions

**Note on Scope**: All proposed optimizations in this report strictly target the **Python Visualization Layer** (`analyze_files.py` and its interaction with Matplotlib). These changes will have **zero impact** on the underlying C analysis algorithms or the accuracy of the transient/resonance scores.

### Strategy 1: Enable True Blitting
Transitioning to `blit=True` would yield the single largest performance gain.
-   **Implementation**: Pre-allocate all static background elements once. The `update()` function should only return the specific artists that have changed.
-   **Impact**: Redrawing only the "dirty" areas of the canvas would likely reduce frame rendering time by 60-80%.

### Strategy 2: Pre-allocated Artist Pools (The "Object Pool" Pattern)
Instead of creating and removing scores/qualifiers on the fly:
-   Create a fixed pool of 50 score labels and 50 qualifier lines during initialization.
-   Hide them by setting `visible=False` or alpha=0.
-   In the `update()` function, simply update the position, text, and visibility of existing pool members.

### Strategy 3: Optimize the Historical Buffer
-   **Fast Fills**: Replace `fill_between` with a single `PolyCollection` or a fast image-based mask if possible.
-   **Static Indices**: Since the 5s buffer has a fixed length of 5001 samples, many rendering parameters (like the X-axis mapping) can be pre-calculated.

### Strategy 4: Offload to a Specialized Visualization Backend

If Matplotlib's charting overhead remains prohibitive, the system could move to a more low-level drawing model.

#### A. Recommendation: Direct Pixel Rendering (OpenCV + MoviePy)
For the specific task of generating an offline MP4 report, this is the **most efficient and recommended approach**.
-   **Benefit**: By using OpenCV to "paint" lines, text, and rectangles directly onto a NumPy array (acting as a pixel buffer) and feeding that to a MoviePy/FFmpeg writer, you bypass the entire "Scene Graph" and "Chart Object" abstraction of Matplotlib.
-   **Benefit**: Extremely fast rendering (often 10-20x faster than Matplotlib) because it uses optimized C/C++ primitives for basic drawing.
-   **Drawback**: You lose "free" features like axis labeling, auto-scaling, and grid lines. These would need to be manually implemented as coordinate-to-pixel mapping functions.

#### B. Alternative: Hardware Accelerated UI (PySide/PyQt + OpenGL)
Best suited if real-time interactive playback within a Python window is required.
-   **Benefit**: Uses the GPU to rasterize the charts, allowing for thousands of simultaneous dynamic artists without dropping frames.
-   **Drawback**: Significantly higher architectural complexity. Requires managing a GUI event loop and writing GLSL or using a high-level GL wrapper like `vispy`.

## 4. Conclusion

The rendering slowness is a classic "UI Overhead" problem. By shifting from a **Reconstructive** model (deleting and rebuilding the scene every frame) to a **Mutative** model (pre-allocating elements and updating their properties using blitting), we can bring the rendering speed into alignment with the newly optimized analysis engine.

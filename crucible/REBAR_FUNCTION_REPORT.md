# Crucible Rebar Function Report

This report documents the implementation of the new `rebar` function inside the `crucible` object, which provides intelligent, robust conversion of transcript bar timestamps to a new `bar_length`.

## 1. Specifications and Requirements

The `rebar` feature is triggered by sending a `rebar` message to the first inlet of the `crucible` object, requiring a single positive integer representing the new `bar_length`.

### Conversion Process:
1. **Timestamp Rounding**: Every bar timestamp key is converted to the closest multiple of the new `bar_length` using standard rounding:
   $$\text{new\_ts} = \text{round}\left(\frac{\text{old\_ts}}{\text{new\_bar\_length}}\right) \times \text{new\_bar\_length}$$
2. **Span Conversion**: The list of timestamps inside the `span` key of each bar is converted using the same rounding formula.
3. **Contiguous Span Padding**: Any missing multiples of the new `bar_length` from the lowest converted bar timestamp to the end of the highest converted bar (exclusive of the end timestamp itself, representing the limiting bounds of where we are looking for missing bars) are filled in and added to the array in low to high order.
4. **Key Construction**: All bar timestamps added to the span are also added outright as keys of their own under the track in question in the dictionary, with identical `span` arrays.
5. **Offset and Palette Copying**: Under each bar key in the newly assembled span, the `offset` and `palette` keys are copied from the nearest pre-conversion bar (minimum timestamp difference) to protect integrity.
6. **Parallel Array Pairing (Scores/Absolutes)**:
   - For each `(absolute, score)` pair found under each pre-conversion bar, the absolute's offset is subtracted and the result is floored to the new `bar_length`:
     $$\text{T\_post} = \text{floor}\left(\frac{\text{absolute} - \text{offset}}{\text{new\_bar\_length}}\right) \times \text{new\_bar\_length}$$
   - If $\text{T\_post}$ is within the newly assembled span, the pair is inserted into the `scores` and `absolutes` arrays of that post-conversion bar.
   - If $\text{T\_post}$ is outside the span, the pair is omitted/forgotten, and a detailed warning is posted directly to the Max console.
7. **Mean Calculation**: For each post-conversion bar, its `mean` key is calculated as the average of all its mapped `scores`. If there are no scores, it defaults to `0.0`.
8. **Rating Recalculation**: For each post-conversion bar, its `rating` is calculated as the minimum `mean` among all bars in its span multiplied by the total number of bars in its span:
   $$\text{rating} = \text{min\_mean} \times \text{new\_span\_len}$$

---

## 2. Visualization and Integration

- **Bar Length Update**: The object's internal stored `local_bar_length` is updated to the new `bar_length`.
- **Reaches Recalculation**: Track reaches and song reaches are recalculated to adapt to the new bar length structure.
- **Subdued Rating Rendering**: If the `@visualize` attribute is enabled, the object sends:
  1. A `repopulate` packet containing the entire re-barred incumbent dictionary structure to update the visualizer's state.
  2. A `replace` packet for every bar of every track to render the updated rating value in a subdued, non-flashing gray style on the visualizer without triggering popping animations.

---

## 3. Implementation Details

The core logic is implemented in `crucible.c` under `crucible_do_rebar`. It leverages:
- Highly surgical array-processing code utilizing parallel sorting (`qsort`) and deduplication.
- Safe deep copying of nested structures via helper functions (`crucible_copy_key`).
- Standard Win32/POSIX cross-platform safe memory allocation APIs (`sysmem_newptr`, `sysmem_freeptr`, `sysmem_resizeptr`).

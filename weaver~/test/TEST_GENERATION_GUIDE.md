# Weaver~ Unit Test Dataset & Transcript Generation Guide

This guide provides the complete specification, mathematical rules, and step-by-step procedures for synthesizing accurate unit test datasets (`transcript.json` and accompanying palette WAV files) for the `weaver~` external and the Max MSP audio system at large.

---

## 1. System Overview & Architecture

The `weaver~` object streams audio from external palette WAV buffers into destination song buffers in real time. It evaluates a time ramp signal corresponding to song playback position $T_{\text{bar}}$ (in milliseconds). At each bar boundary, `weaver~` performs a lookup in a `transcript.json` dictionary to determine:
1. Which palette WAV buffer to reference (the `"palette"` key).
2. The millisecond offset anchor in that palette file (the `"offset"` key).
3. The note onset events (`"absolutes"`) and ratings (`"rating"`).

---

## 2. Core Mathematical Rules & Alignment Equations

### Rule 1: Audio Playback Read Position
When `weaver~` plays bar $T_{\text{bar}}$ (e.g. $T_{\text{bar}} = 28000\text{ ms}$) with transcript offset $O$ (e.g. $O = 16000\text{ ms}$), the read head in the palette WAV file reads audio starting at:
$$\text{Palette Audio Read Position} = T_{\text{bar}} + O$$

For example, for bar $T_{\text{bar}} = 28000\text{ ms}$ and offset $O = 16000\text{ ms}$, `weaver~` reads palette audio starting at $28000 + 16000 = 44000\text{ ms}$.

### Rule 2: Palette Audio Alignment (Zero Static)
To guarantee that 0% white noise static is heard during song playback:
- For every bar $T_{\text{bar}}$ in `transcript.json` referencing palette $P$ with offset $O$, clean synthesized drum beat audio **must** be written into palette WAV file $P$ at the audio window:
$$\text{Audio Window} = [T_{\text{bar}} + O,\; T_{\text{bar}} + O + \text{BAR\_MS})$$
- All unreferenced audio regions in palette WAV files are filled with white noise static (amplitude $\sim 0.15$).

### Rule 3: Note Absolutes & Flooring
For any note onset in bar $T_{\text{bar}}$ occurring at intra-bar offset $t_{\text{note\_off}} \in [0, \text{BAR\_MS})$, the note's absolute timestamp $A$ in `transcript.json` is:
$$A = T_{\text{bar}} + O + t_{\text{note\_off}}$$

When the visualizer or DSP engine computes $(A - O)$:
$$A - O = (T_{\text{bar}} + O + t_{\text{note\_off}}) - O = T_{\text{bar}} + t_{\text{note\_off}}$$

Flooring $(A - O)$ by bar duration $\text{BAR\_MS}$ yields:
$$\left\lfloor \frac{A - O}{\text{BAR\_MS}} \right\rfloor \times \text{BAR\_MS} = T_{\text{bar}}$$

This guarantees that every note's $(A - O)$ floors **exactly** into the bar $T_{\text{bar}}$ it is a part of.

### Rule 4: Palette Filename Stripping
- **On Disk:** Palette WAV files are named with the `palette_` prefix (e.g. `palette_1.wav`, `palette_2.wav`, `palette_3.wav`).
- **In `transcript.json`:** The `"palette"` key strips the `palette_` prefix (e.g. `"1.wav"`, `"2.wav"`, `"3.wav"`).

---

## 3. Transcript JSON Schema

The `transcript.json` file is structured as a nested dictionary:
```json
{
    "1": {
        "0": {
            "absolutes": [10000.0, 10500.0, 11000.0, 11500.0],
            "scores": [0.12, 0.25, 0.18, 0.3],
            "offset": 10000.0,
            "mean": 0.2125,
            "span": 0,
            "rating": 1.0,
            "palette": "1.wav"
        },
        "2000": {
            "absolutes": [12000.0, 12500.0, 13000.0, 13500.0],
            "scores": [0.12, 0.25, 0.18, 0.3],
            "offset": 10000.0,
            "mean": 0.2125,
            "span": [2000, 4000, 6000],
            "rating": 1.2,
            "palette": "1.wav"
        }
    }
}
```

### JSON Fields Per Bar
- `"absolutes"`: List of float timestamps in ms representing note onset events in the palette audio space ($A = T_{\text{bar}} + O + t_{\text{note\_off}}$).
- `"scores"`: List of transience scores (floats between $0.05$ and $0.5$) aligned 1-to-1 with `"absolutes"`.
- `"offset"`: Float offset anchor in ms for the palette file.
- `"mean"`: Average score of the note onsets in that bar.
- `"span"`:
  - Single integer (e.g. `0`) if the bar is an isolated single-bar reference.
  - Array of bar timestamps (e.g. `[2000, 4000, 6000]`) if the bar belongs to a multi-bar span.
- `"rating"`: Float rating for dynamic gain and analysis evaluation (e.g. $0.8$ to $1.5$).
- `"palette"`: Palette filename string stripped of `palette_` prefix (e.g. `"1.wav"`).

---

## 4. Dataset Generation Tool

The script `generate_test_data.py` automates the synthesis of all WAV files and `transcript.json`.

### Generator Features
- **Duration:** 2 minutes (120,000 ms = 60 bars at 120 BPM, 2.0s per bar).
- **Tracks:** 4 tracks (`"1"`, `"2"`, `"3"`, `"4"`).
- **Palette Audio:** 3 WAV files (`palette_1.wav`, `palette_2.wav`, `palette_3.wav`), each 160 seconds (80 bars) long.
- **Audio Alignment:** Synthesizes clean punchy drum beats (kick, snare, hi-hats) at exact $[T_{\text{bar}} + O, T_{\text{bar}} + O + \text{BAR\_MS})$ audio windows, filling all unreferenced regions with white noise static.
- **Coverage:** 100% of drum beat audio regions are referenced, and 0% static is played back during song performance.

---

## 5. Step-by-Step Instructions to Create a New Arbitrary Test Set

To create or update a unit test dataset in a future session:

### Step 1: Execute the Generator Script
In bash, navigate to `weaver~/test/` and run `generate_test_data.py`:
```bash
cd "weaver~/test"
python3 generate_test_data.py
```

### Step 2: Verify the Generated Dataset
Run the automated verification test:
```bash
python3 -c "
import json, math, wave, os, struct

test_dir = 'weaver~/test'
with open(os.path.join(test_dir, 'transcript.json')) as f:
    t = json.load(f)

for tr_key, track in t.items():
    for bar_str, bar in track.items():
        expected_bar_ms = int(bar_str)
        offset = float(bar['offset'])
        pal_key = bar['palette']
        full_pal_fn = os.path.join(test_dir, f'palette_{pal_key}')
        
        for a in bar['absolutes']:
            rel_ms = a - offset
            floored_bar_ms = int(math.floor(rel_ms / 2000.0) * 2000)
            assert floored_bar_ms == expected_bar_ms
"
```

All generated test files (`transcript.json`, `palette_1.wav`, `palette_2.wav`, `palette_3.wav`, `generate_test_data.py`, and `TEST_GENERATION_GUIDE.md`) reside inside `weaver~/test/`.

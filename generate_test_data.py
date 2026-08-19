#!/usr/bin/env python3
"""
generate_test_data.py

Synthesizes palette WAV files and corresponding transcript.json file
for unit testing the Max patch and weaver~/crucible system.

- Transcript duration: 2 minutes (120,000 ms = 60 bars of 2000 ms per track) across 4 tracks.
- Palette WAV files: palette_1.wav, palette_2.wav, palette_3.wav (160 seconds each).
- Palette WAV files contain white noise static everywhere EXCEPT at the exact audio positions
  where drum beat segments are referenced by the transcript.
- "palette" key in transcript.json strips "palette_" prefix (e.g., "1.wav", "2.wav", "3.wav").
- Note absolutes are calculated as seg.offset_ms + current_bar_ms + note_intra_bar_offset,
  ensuring (absolute - offset) = current_bar_ms + note_intra_bar_offset,
  which floors directly into current_bar_ms.
- 100% of beat segments are referenced, and 0% static is included in the transcript.
"""

import math
import random
import struct
import wave
import json
from pathlib import Path

SAMPLE_RATE = 44100
BAR_MS = 2000  # 2.0 seconds per bar (120 BPM, 4/4)
BAR_SAMPLES = int(SAMPLE_RATE * (BAR_MS / 1000.0))  # 88,200 samples
SONG_DURATION_MS = 120000  # 2 minutes = 120,000 ms
NUM_SONG_BARS = SONG_DURATION_MS // BAR_MS  # 60 bars per track
NUM_TRACKS = 4
PALETTE_TOTAL_BARS = 80  # 160 seconds per palette WAV file

# --- Drum Synthesis Functions ---

def generate_kick(num_samples):
    """Synthesizes a punchy kick drum sample array."""
    samples = []
    for i in range(num_samples):
        t = i / SAMPLE_RATE
        env = math.exp(-15.0 * t)
        freq = 150.0 * math.exp(-30.0 * t) + 45.0
        val = math.sin(2.0 * math.pi * freq * t) * env
        samples.append(val)
    return samples

def generate_snare(num_samples):
    """Synthesizes a snare drum sample array."""
    samples = []
    for i in range(num_samples):
        t = i / SAMPLE_RATE
        env = math.exp(-20.0 * t)
        tone = math.sin(2.0 * math.pi * 180.0 * t) * 0.4
        noise = random.uniform(-1.0, 1.0) * 0.6
        val = (tone + noise) * env
        samples.append(val)
    return samples

def generate_hihat(num_samples):
    """Synthesizes a hi-hat sample array."""
    samples = []
    for i in range(num_samples):
        t = i / SAMPLE_RATE
        env = math.exp(-60.0 * t)
        noise = random.uniform(-1.0, 1.0)
        val = noise * env * 0.5
        samples.append(val)
    return samples

def synthesize_bar_beat(pattern_id, bar_index_in_segment):
    """
    Synthesizes a single bar (88,200 samples) of steady 4/4 drum beat.
    pattern_id determines the style / fill variations.
    """
    bar_samples = [0.0] * BAR_SAMPLES
    beat_samples = int(SAMPLE_RATE * 0.5)  # 22,050 samples per quarter note
    eighth_samples = beat_samples // 2      # 11,025 samples

    # Generate basic components
    kick = generate_kick(int(SAMPLE_RATE * 0.4))
    snare = generate_snare(int(SAMPLE_RATE * 0.3))
    hihat = generate_hihat(int(SAMPLE_RATE * 0.15))

    def add_hit(hit_samples, start_pos, gain=1.0):
        for idx, s in enumerate(hit_samples):
            pos = start_pos + idx
            if pos < BAR_SAMPLES:
                bar_samples[pos] += s * gain

    p_type = pattern_id % 4

    # Hi-hats on 8th notes
    for eighth in range(8):
        pos = eighth * eighth_samples
        gain = 0.8 if eighth % 2 == 0 else 0.5
        add_hit(hihat, pos, gain)

    # Kicks and Snares
    if p_type == 0:
        add_hit(kick, 0 * beat_samples)
        add_hit(snare, 1 * beat_samples)
        add_hit(kick, 2 * beat_samples)
        add_hit(snare, 3 * beat_samples)
    elif p_type == 1:
        add_hit(kick, 0 * beat_samples)
        add_hit(kick, 0 * beat_samples + eighth_samples)
        add_hit(snare, 1 * beat_samples)
        add_hit(kick, 2 * beat_samples + eighth_samples)
        add_hit(snare, 3 * beat_samples)
    elif p_type == 2:
        add_hit(kick, 0 * beat_samples)
        add_hit(snare, 1 * beat_samples)
        add_hit(kick, 2 * beat_samples)
        add_hit(kick, 2 * beat_samples + eighth_samples)
        add_hit(snare, 3 * beat_samples)
    else:
        add_hit(kick, 0 * beat_samples)
        add_hit(snare, 1 * beat_samples)
        add_hit(kick, 2 * beat_samples)
        # Snare fill on beat 4
        add_hit(snare, 3 * beat_samples)
        add_hit(snare, 3 * beat_samples + eighth_samples, 0.7)

    return bar_samples


# --- Palette Definitions ---

# Defining candidate palette offset locations (in ms) and lengths
PALETTE_SPECS = [
    {
        "filename": "palette_1.wav",
        "offset_ms": 10000,
        "segments": [
            (1, 0),  # 1 bar single
            (3, 1),  # 3 bar span
            (2, 2),  # 2 bar span
            (1, 3),  # 1 bar single
            (4, 0),  # 4 bar span
            (2, 1),  # 2 bar span
            (1, 2),  # 1 bar single
            (5, 3),  # 5 bar span
            (2, 0),  # 2 bar span
        ]
    },
    {
        "filename": "palette_2.wav",
        "offset_ms": 16000,
        "segments": [
            (2, 3),
            (1, 0),
            (4, 1),
            (1, 2),
            (3, 3),
            (2, 0),
            (1, 1),
            (2, 2),
            (4, 0),
            (1, 3),
        ]
    },
    {
        "filename": "palette_3.wav",
        "offset_ms": 12000,
        "segments": [
            (3, 0),
            (1, 1),
            (2, 2),
            (5, 3),
            (1, 0),
            (2, 1),
            (1, 2),
            (1, 3),
            (3, 1),
            (2, 2),
        ]
    }
]


class SegmentSpec:
    def __init__(self, palette_name, offset_ms, num_bars, pattern_id):
        self.palette_name = palette_name
        self.offset_ms = offset_ms
        self.num_bars = num_bars
        self.pattern_id = pattern_id


def build_transcript_and_palette_audio():
    """
    Builds 4 tracks over 2 minutes (60 bars per track) in transcript.json.
    Simultaneously populates audio in palette_1.wav, palette_2.wav, palette_3.wav
    at exactly seg.offset_ms + current_bar_ms, filling all other areas with static.
    """
    transcript = {}

    # Total audio array for each palette WAV file (80 bars = 160 seconds)
    total_palette_samples = PALETTE_TOTAL_BARS * BAR_SAMPLES
    palette_audio_buffers = {
        spec["filename"]: [random.uniform(-0.15, 0.15) for _ in range(total_palette_samples)]
        for spec in PALETTE_SPECS
    }

    # Gather available segment templates
    all_segment_specs = []
    for spec in PALETTE_SPECS:
        fn = spec["filename"]
        base_off = spec["offset_ms"]
        for (num_bars, pattern_id) in spec["segments"]:
            all_segment_specs.append(SegmentSpec(fn, base_off, num_bars, pattern_id))

    unreferenced_segments = list(all_segment_specs)
    all_segment_pool = list(all_segment_specs)

    def get_next_segment():
        nonlocal unreferenced_segments
        if unreferenced_segments:
            return unreferenced_segments.pop(0)
        else:
            return random.choice(all_segment_pool)

    referenced_segments = set()

    for tr_num in range(1, NUM_TRACKS + 1):
        tr_key = str(tr_num)
        track_dict = {}
        bar_idx = 0

        while bar_idx < NUM_SONG_BARS:
            seg = get_next_segment()
            referenced_segments.add((seg.palette_name, seg.offset_ms, seg.num_bars))

            bars_to_place = seg.num_bars
            if bar_idx + bars_to_place > NUM_SONG_BARS:
                bars_to_place = 1

            span_bars = [ (bar_idx + b) * BAR_MS for b in range(bars_to_place) ]

            palette_key = seg.palette_name
            if palette_key.startswith("palette_"):
                palette_key = palette_key[len("palette_"):]

            for b in range(bars_to_place):
                current_bar_ms = (bar_idx + b) * BAR_MS
                bar_key = str(current_bar_ms)

                # 1. Generate note absolutes in transcript.json
                # absolutes = seg.offset_ms + current_bar_ms + note_intra_bar_offset
                # So (absolute - offset) = current_bar_ms + note_intra_bar_offset
                # floor((absolute - offset) / 2000) * 2000 == current_bar_ms
                absolutes = [ float(seg.offset_ms + current_bar_ms + note_off) for note_off in [0, 500, 1000, 1500] ]
                scores = [ 0.12, 0.25, 0.18, 0.30 ]
                mean_score = sum(scores) / len(scores)

                bar_entry = {
                    "absolutes": absolutes,
                    "scores": scores,
                    "offset": float(seg.offset_ms),
                    "mean": mean_score,
                    "span": span_bars if bars_to_place > 1 else current_bar_ms,
                    "rating": round(0.8 + 0.2 * (seg.pattern_id + 1), 2),
                    "palette": palette_key
                }
                track_dict[bar_key] = bar_entry

                # 2. Write drum beat audio into palette audio buffer
                # At palette audio offset: seg.offset_ms + current_bar_ms
                palette_write_start_ms = seg.offset_ms + current_bar_ms
                start_sample = int((palette_write_start_ms / 1000.0) * SAMPLE_RATE)
                bar_audio = synthesize_bar_beat(seg.pattern_id, b)

                buf = palette_audio_buffers[seg.palette_name]
                for s_idx, sample_val in enumerate(bar_audio):
                    target_sample = start_sample + s_idx
                    if target_sample < len(buf):
                        buf[target_sample] = sample_val

            bar_idx += bars_to_place

        transcript[tr_key] = track_dict

    # Write palette WAV files to disk
    for fn, buf in palette_audio_buffers.items():
        out_path = Path(fn)
        with wave.open(str(out_path), "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(SAMPLE_RATE)
            packed_frames = bytearray()
            for sample in buf:
                clamped = max(-1.0, min(1.0, sample))
                int_val = int(clamped * 32767.0)
                packed_frames.extend(struct.pack("<h", int_val))
            wav_file.writeframes(packed_frames)
        print(f"Generated {fn} ({PALETTE_TOTAL_BARS * (BAR_MS/1000.0):.1f}s)")

    # Verify all segments referenced
    for seg in all_segment_specs:
        if (seg.palette_name, seg.offset_ms, seg.num_bars) not in referenced_segments:
            print(f"WARNING: Segment {seg.palette_name} @ {seg.offset_ms}ms was not referenced!")

    return transcript


def main():
    print("Synthesizing audio palette files and transcript dictionary...")
    transcript_data = build_transcript_and_palette_audio()

    out_path = Path("transcript.json")
    with open(out_path, "w") as f:
        json.dump(transcript_data, f, indent=4)
    print(f"Wrote {out_path.name}")

    print("Test data generation complete!")


if __name__ == "__main__":
    main()

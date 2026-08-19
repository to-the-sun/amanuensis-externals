#!/usr/bin/env python3
"""
generate_test_data.py

Synthesizes palette WAV files and corresponding transcript.json / sampletranscript.json files
for unit testing the Max patch and weaver~/crucible system.

Palette WAV files are mostly white noise static, with exact bar-length drum beat segments placed
at specific offsets throughout them. The transcript file maps 4 tracks over a 60-second song duration
exclusively to these beat segments (using single-bar references and multi-bar spans).
100% of beat segments are referenced, and 0% static is included in the transcript.
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
SONG_DURATION_MS = 60000  # 60 seconds
NUM_SONG_BARS = SONG_DURATION_MS // BAR_MS  # 30 bars per track
NUM_TRACKS = 4

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

    # Pattern definitions based on pattern_id
    # 1. Standard 4/4: Kick on 1 & 3, Snare on 2 & 4, Hihat on all 8ths
    # 2. Syncopated: Kick on 1, 1&, 3; Snare on 2, 4
    # 3. Driving: Kick on 1, 2, 3, 4; Snare on 2, 4
    # 4. Fill variation: Snare fill on beat 4

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

# Each palette file definition specifies:
# filename, total_bars, and a list of beat segments: (start_bar, num_bars, pattern_id)
PALETTE_SPECS = [
    {
        "filename": "palette_1.wav",
        "total_bars": 35,  # 70 seconds
        "segments": [
            (2, 1, 0),   # 1 bar at 4000ms
            (6, 3, 1),   # 3 bars at 12000ms (12000, 14000, 16000)
            (11, 2, 2),  # 2 bars at 22000ms (22000, 24000)
            (15, 1, 3),  # 1 bar at 30000ms
            (19, 4, 0),  # 4 bars at 38000ms (38000, 40000, 42000, 44000)
            (25, 2, 1),  # 2 bars at 50000ms (50000, 52000)
            (30, 1, 2),  # 1 bar at 60000ms
        ]
    },
    {
        "filename": "palette_2.wav",
        "total_bars": 35,  # 70 seconds
        "segments": [
            (1, 2, 3),   # 2 bars at 2000ms
            (5, 1, 0),   # 1 bar at 10000ms
            (8, 4, 1),   # 4 bars at 16000ms
            (14, 1, 2),  # 1 bar at 28000ms
            (17, 3, 3),  # 3 bars at 34000ms
            (22, 2, 0),  # 2 bars at 44000ms
            (26, 1, 1),  # 1 bar at 52000ms
            (30, 2, 2),  # 2 bars at 60000ms
        ]
    },
    {
        "filename": "palette_3.wav",
        "total_bars": 35,  # 70 seconds
        "segments": [
            (2, 3, 0),   # 3 bars at 4000ms
            (7, 1, 1),   # 1 bar at 14000ms
            (10, 2, 2),  # 2 bars at 20000ms
            (14, 5, 3),  # 5 bars at 28000ms
            (21, 1, 0),  # 1 bar at 42000ms
            (24, 2, 1),  # 2 bars at 48000ms
            (28, 1, 2),  # 1 bar at 56000ms
            (31, 1, 3),  # 1 bar at 62000ms
        ]
    }
]


class SegmentInfo:
    def __init__(self, palette_name, offset_ms, num_bars, pattern_id):
        self.palette_name = palette_name
        self.offset_ms = offset_ms
        self.num_bars = num_bars
        self.pattern_id = pattern_id


def build_palette_audio_files():
    """Generates the WAV files according to PALETTE_SPECS."""
    all_segments = []

    for spec in PALETTE_SPECS:
        fn = spec["filename"]
        total_bars = spec["total_bars"]
        total_samples = total_bars * BAR_SAMPLES
        audio = [0.0] * total_samples

        # Fill background with static (white noise)
        for i in range(total_samples):
            audio[i] = random.uniform(-0.15, 0.15)

        # Overwrite drum beat segments
        for (start_bar, num_bars, pattern_id) in spec["segments"]:
            offset_ms = start_bar * BAR_MS
            segment_obj = SegmentInfo(fn, offset_ms, num_bars, pattern_id)
            all_segments.append(segment_obj)

            for b in range(num_bars):
                bar_audio = synthesize_bar_beat(pattern_id, b)
                start_sample = (start_bar + b) * BAR_SAMPLES
                for s_idx, sample_val in enumerate(bar_audio):
                    audio[start_sample + s_idx] = sample_val

        # Write WAV file (16-bit PCM Mono)
        out_path = Path(fn)
        with wave.open(str(out_path), "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(SAMPLE_RATE)
            packed_frames = bytearray()
            for sample in audio:
                # Clamp sample to [-1.0, 1.0]
                clamped = max(-1.0, min(1.0, sample))
                int_val = int(clamped * 32767.0)
                packed_frames.extend(struct.pack("<h", int_val))
            wav_file.writeframes(packed_frames)
        print(f"Generated {fn} ({total_bars * (BAR_MS/1000.0):.1f}s, {len(spec['segments'])} beat segments)")

    return all_segments


def create_transcript_dictionary(all_segments):
    """
    Constructs transcript JSON mapping 4 tracks over 60 seconds (30 bars per track)
    using all beat segments from all_segments.
    Guarantees 100% beat segment coverage and 0% static inclusion.
    """
    transcript = {}

    # Track data layout: 4 tracks, each has 30 bars (timestamps 0, 2000, 4000, ..., 58000 ms)
    # We will tile all segments into song bars across tracks.

    # Maintain a list of available segments to ensure all are referenced at least once.
    unreferenced_segments = list(all_segments)
    all_segments_pool = list(all_segments)

    def get_next_segment():
        nonlocal unreferenced_segments
        if unreferenced_segments:
            return unreferenced_segments.pop(0)
        else:
            return random.choice(all_segments_pool)

    for tr_num in range(1, NUM_TRACKS + 1):
        tr_key = str(tr_num)
        track_dict = {}
        bar_idx = 0

        while bar_idx < NUM_SONG_BARS:
            seg = get_next_segment()
            # Decide if we place this as a multi-bar span or single bar
            # If segment has seg.num_bars > 1 and fits in remaining bars:
            bars_to_place = seg.num_bars
            if bar_idx + bars_to_place > NUM_SONG_BARS:
                bars_to_place = 1  # clamp or place single bar

            song_start_ms = bar_idx * BAR_MS
            span_bars = [ (bar_idx + b) * BAR_MS for b in range(bars_to_place) ]

            for b in range(bars_to_place):
                current_bar_ms = (bar_idx + b) * BAR_MS
                bar_key = str(current_bar_ms)

                # Generate onset absolutes and scores
                # Onset times relative to song bar start: 0, 500, 1000, 1500 ms + bar start
                absolutes = [ float(current_bar_ms + offset) for offset in [0, 500, 1000, 1500] ]
                scores = [ 0.12, 0.25, 0.18, 0.30 ]
                mean_score = sum(scores) / len(scores)

                bar_entry = {
                    "absolutes": absolutes,
                    "scores": scores,
                    "offset": float(seg.offset_ms),
                    "mean": mean_score,
                    "span": span_bars if bars_to_place > 1 else current_bar_ms,
                    "rating": round(0.8 + 0.2 * (seg.pattern_id + 1), 2),
                    "palette": seg.palette_name
                }
                track_dict[bar_key] = bar_entry

            bar_idx += bars_to_place

        transcript[tr_key] = track_dict

    # Verify that all segments are referenced
    referenced_keys = set()
    for tr_dict in transcript.values():
        for bar_data in tr_dict.values():
            pal = bar_data["palette"]
            off = bar_data["offset"]
            referenced_keys.add((pal, off))

    for seg in all_segments:
        if (seg.palette_name, seg.offset_ms) not in referenced_keys:
            print(f"WARNING: Segment {seg.palette_name} @ {seg.offset_ms}ms was not referenced!")

    return transcript


def main():
    print("Synthesizing audio palette files...")
    all_segments = build_palette_audio_files()

    print("Generating transcript dictionary...")
    transcript_data = create_transcript_dictionary(all_segments)

    # Save to both transcript.json and sampletranscript.json
    for fn in ["transcript.json", "sampletranscript.json"]:
        out_path = Path(fn)
        with open(out_path, "w") as f:
            json.dump(transcript_data, f, indent=4)
        print(f"Wrote {fn}")

    print("Test data generation complete!")


if __name__ == "__main__":
    main()

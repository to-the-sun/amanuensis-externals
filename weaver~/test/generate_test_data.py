#!/usr/bin/env python3
"""
generate_test_data.py

Synthesizes control test transcript.json and palette WAV file (palette_controlbeat.wav).
- 4 tracks ("1", "2", "3", "4")
- Each track has a bar at timestamp 0 ("0")
- Track 1 has 2 minutes of bars in the negative direction (-120000 ms to 0 ms, 60 negative bars + bar 0)
- Synthesizes palette_controlbeat.wav containing a 120 BPM steady drum beat (kick, snare, hi-hat pattern)
"""

import os
import json
import wave
import struct
import math
import random

def generate_palette_wav(filepath, duration_sec=8.0, sr=44100, bpm=120):
    beat_sec = 60.0 / bpm  # 0.5s = 500ms
    samples_per_beat = int(sr * beat_sec)
    total_samples = int(sr * duration_sec)

    num_channels = 2
    samples = [[0.0, 0.0] for _ in range(total_samples)]

    # Generate steady beat pattern across the total samples
    num_beats = int(duration_sec / beat_sec)
    for b in range(num_beats):
        start_idx = b * samples_per_beat
        beat_type = b % 4  # 0: Kick, 1: Snare, 2: Kick+Hat, 3: Snare

        # 1. Kick on beat 0 and 2
        if beat_type in (0, 2):
            kick_len = int(sr * 0.15)  # 150ms
            for i in range(min(kick_len, total_samples - start_idx)):
                t = i / sr
                freq = 60.0 * math.exp(-t * 20.0) + 30.0
                env = math.exp(-t * 15.0)
                val = math.sin(2.0 * math.pi * freq * t) * env * 0.8
                samples[start_idx + i][0] += val
                samples[start_idx + i][1] += val

        # 2. Snare on beat 1 and 3
        if beat_type in (1, 3):
            snare_len = int(sr * 0.12)  # 120ms
            for i in range(min(snare_len, total_samples - start_idx)):
                t = i / sr
                env = math.exp(-t * 25.0)
                tone = math.sin(2.0 * math.pi * 180.0 * t) * 0.4
                noise = (random.random() * 2.0 - 1.0) * 0.5
                val = (tone + noise) * env * 0.7
                samples[start_idx + i][0] += val
                samples[start_idx + i][1] += val

        # 3. Hi-hat on every beat
        hat_len = int(sr * 0.04)  # 40ms
        for i in range(min(hat_len, total_samples - start_idx)):
            t = i / sr
            env = math.exp(-t * 80.0)
            noise = (random.random() * 2.0 - 1.0) * 0.25
            samples[start_idx + i][0] += noise * env
            samples[start_idx + i][1] += noise * env

    # Normalize samples to avoid clipping
    max_amp = max(max(abs(s[0]), abs(s[1])) for s in samples)
    if max_amp > 0:
        scale = 0.85 / max_amp
        for s in samples:
            s[0] *= scale
            s[1] *= scale

    # Write WAV file (16-bit PCM stereo)
    with wave.open(filepath, 'w') as wf:
        wf.setnchannels(num_channels)
        wf.setsampwidth(2)
        wf.setframerate(sr)

        packed_data = bytearray()
        for s in samples:
            left = int(max(-32768, min(32767, s[0] * 32767)))
            right = int(max(-32768, min(32767, s[1] * 32767)))
            packed_data.extend(struct.pack('<hh', left, right))

        wf.writeframes(packed_data)

def generate_transcript_json(filepath):
    bar_length = 2000.0  # 2000 ms per bar (4 beats @ 120 BPM)
    palette_name = "controlbeat.wav"  # Reference palette without "palette_" prefix, no underscore in filename after palette_

    transcript = {}

    # 4 Tracks: "1", "2", "3", "4"
    # Track 1: 2 minutes in negative direction (-120000 ms to 0 ms)
    # 2 mins = 120 seconds = 120000 ms = 60 bars of 2000ms below zero (-120000, -118000, ..., -2000, 0)
    track1_bars = list(range(-120000, 2000, 2000))  # -120000 to 0 inclusive

    tracks_bar_map = {
        "1": track1_bars,
        "2": [0],
        "3": [0],
        "4": [0]
    }

    for track_id, bar_timestamps in tracks_bar_map.items():
        track_dict = {}
        for b_ts in bar_timestamps:
            b_key = str(b_ts)

            # 4 beats per bar (every 500ms)
            absolutes = [float(b_ts + i * 500) for i in range(4)]
            scores = [0.85, 0.90, 0.88, 0.92]
            mean_score = sum(scores) / len(scores)

            bar_dict = {
                "absolutes": absolutes,
                "scores": scores,
                "mean": round(mean_score, 4),
                "offset": 0.0,
                "palette": palette_name,
                "rating": 1.0,
                "span": [b_ts]
            }
            track_dict[b_key] = bar_dict

        transcript[track_id] = track_dict

    with open(filepath, 'w') as f:
        json.dump(transcript, f, indent=2)

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))

    wav_path = os.path.join(script_dir, "palette_controlbeat.wav")
    json_path = os.path.join(script_dir, "transcript.json")

    print(f"Generating palette WAV at {wav_path}...")
    generate_palette_wav(wav_path)

    print(f"Generating transcript JSON at {json_path}...")
    generate_transcript_json(json_path)

    print("Generation complete!")

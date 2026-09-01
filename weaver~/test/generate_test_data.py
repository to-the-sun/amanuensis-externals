#!/usr/bin/env python3
"""
generate_test_data.py

Synthesizes control test audio files and transcript.json in weaver~/test/:
- 4 stem WAV files (stems_1.wav, stems_2.wav, stems_3.wav, stems_4.wav) ~30s each filled with white noise.
- 1 palette WAV file (palette_warbling_bass.wav) filled with a deep and continuous warbling bass synth.
- transcript.json for tracks "1", "2", "3", "4" alternating bar by bar between:
  1) referencing valid palette WAV ("warbling_bass.wav")
  2) completely missing bar
  3) referencing non-existent palette WAV ("nonexistent.wav")
"""

import os
import json
import wave
import struct
import math
import random

def generate_stems_wav(filepath, duration_sec=30.0, sr=44100):
    total_samples = int(sr * duration_sec)
    num_channels = 2

    packed_data = bytearray()
    for _ in range(total_samples):
        # White noise in range [-0.5, 0.5]
        left_val = (random.random() * 2.0 - 1.0) * 0.5
        right_val = (random.random() * 2.0 - 1.0) * 0.5

        left = int(max(-32768, min(32767, left_val * 32767)))
        right = int(max(-32768, min(32767, right_val * 32767)))
        packed_data.extend(struct.pack('<hh', left, right))

    with wave.open(filepath, 'w') as wf:
        wf.setnchannels(num_channels)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        wf.writeframes(packed_data)

def generate_palette_bass_wav(filepath, duration_sec=35.0, sr=44100):
    total_samples = int(sr * duration_sec)
    num_channels = 2

    base_freq = 45.0  # Deep bass pitch (Hz)
    lfo_rate = 4.5    # Warble frequency LFO (Hz)
    lfo_depth = 10.0  # Warble frequency depth (+/- Hz)
    amp_lfo_rate = 3.0 # Amplitude modulation LFO (Hz)

    phase = 0.0
    packed_data = bytearray()

    for i in range(total_samples):
        t = i / sr

        # Frequency modulation (pitch warble)
        inst_freq = base_freq + lfo_depth * math.sin(2.0 * math.pi * lfo_rate * t)
        phase += 2.0 * math.pi * inst_freq / sr

        # Rich sub-bass with harmonics (fundamental + 2nd harmonic + 3rd harmonic)
        raw_wave = math.sin(phase) + 0.5 * math.sin(2.0 * phase) + 0.25 * math.sin(3.0 * phase)

        # Amplitude modulation (tremolo / sub-bass wobble)
        amp_mod = 0.7 + 0.3 * math.sin(2.0 * math.pi * amp_lfo_rate * t)

        val = raw_wave * amp_mod * 0.5

        left = int(max(-32768, min(32767, val * 32767)))
        right = int(max(-32768, min(32767, val * 32767)))
        packed_data.extend(struct.pack('<hh', left, right))

    with wave.open(filepath, 'w') as wf:
        wf.setnchannels(num_channels)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        wf.writeframes(packed_data)

def generate_transcript_json(filepath, num_bars=15, bar_length_ms=2000.0):
    """
    Generate transcript.json for 4 tracks ("1", "2", "3", "4") over 15 bars (30 seconds @ 120 BPM).
    Alternates bar by bar:
      Bar idx % 3 == 0: Referencing valid palette ("warbling_bass.wav")
      Bar idx % 3 == 1: Completely missing bar (omitted)
      Bar idx % 3 == 2: Referencing non-existent palette ("nonexistent.wav")
    """
    valid_palette = "warbling_bass.wav"
    nonexistent_palette = "nonexistent.wav"

    transcript = {}

    for track_num in range(1, 5):
        track_id = str(track_num)
        track_dict = {}

        for b in range(num_bars):
            bar_ts = int(b * bar_length_ms)
            b_key = str(bar_ts)

            pattern_idx = b % 3

            if pattern_idx == 0:
                palette_ref = valid_palette
            elif pattern_idx == 1:
                # Completely missing bar - omit from transcript dictionary
                continue
            else: # pattern_idx == 2
                palette_ref = nonexistent_palette

            # 4 beats per bar (every 500ms)
            absolutes = [float(bar_ts + i * 500) for i in range(4)]
            scores = [0.85, 0.90, 0.88, 0.92]
            mean_score = sum(scores) / len(scores)

            bar_dict = {
                "absolutes": absolutes,
                "scores": scores,
                "mean": round(mean_score, 4),
                "offset": 0.0,
                "palette": palette_ref,
                "rating": 1.0,
                "span": [bar_ts]
            }
            track_dict[b_key] = bar_dict

        transcript[track_id] = track_dict

    with open(filepath, 'w') as f:
        json.dump(transcript, f, indent=2)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    # 1. Generate 4 stem WAV files filled with white noise
    for i in range(1, 5):
        stem_path = os.path.join(script_dir, f"stems_{i}.wav")
        print(f"Generating stem WAV at {stem_path}...")
        generate_stems_wav(stem_path, duration_sec=30.0)

    # 2. Generate palette WAV with warbling bass synth
    palette_path = os.path.join(script_dir, "palette_warbling_bass.wav")
    print(f"Generating palette WAV at {palette_path}...")
    generate_palette_bass_wav(palette_path, duration_sec=35.0)

    # 3. Generate transcript.json
    json_path = os.path.join(script_dir, "transcript.json")
    print(f"Generating transcript JSON at {json_path}...")
    generate_transcript_json(json_path)

    print("All control test files generated successfully!")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
remove_silent_bars.py

Looks for transcript.json three folder levels up from the script's directory.
Scans all WAV files in the script's directory starting with prefixes '01', '02', '03', and '04',
correlating them with tracks 1, 2, 3, and 4 in transcript.json.

For each WAV file, identifies bar-length sections corresponding to all bars in the transcript dictionary.
Takes into account that transcript bar timestamps can be negative while audio in WAV files is 0-indexed:
  WAV_start_ms = bar_timestamp - most_negative_bar

If a bar-length audio section in a WAV file is silent, removes the associated bar from transcript.json
and prints an informative message stating both the absolute WAV file song time and the transcript timestamp.

Keeps the console open upon completion.
"""

import os
import sys
import wave
import struct
import json
from pathlib import Path


def format_time_str(ms):
    """
    Formats milliseconds into a readable string like '-0:04' or '1:15' (m:ss).
    """
    is_neg = ms < 0
    abs_sec = abs(ms) / 1000.0
    minutes = int(abs_sec // 60)
    seconds = abs_sec % 60
    sign = "-" if is_neg else ""
    return f"{sign}{minutes}:{seconds:04.1f}"


def get_most_negative_bar(data):
    """
    Finds the lowest (most negative) bar timestamp across all tracks in the transcript data.
    Returns 0.0 if no negative bar timestamps exist.
    """
    most_neg = 0.0
    for track_id, track_data in data.items():
        if not isinstance(track_data, dict):
            continue
        for bar_key, bar_dict in track_data.items():
            bar_ts = None
            try:
                bar_ts = float(bar_key)
            except ValueError:
                pass

            if bar_ts is None and isinstance(bar_dict, dict):
                if "span" in bar_dict and isinstance(bar_dict["span"], list) and len(bar_dict["span"]) > 0:
                    bar_ts = float(bar_dict["span"][0])
                elif "absolutes" in bar_dict and isinstance(bar_dict["absolutes"], list) and len(bar_dict["absolutes"]) > 0:
                    bar_ts = float(bar_dict["absolutes"][0])

            if bar_ts is not None and bar_ts < most_neg:
                most_neg = bar_ts
    return most_neg


def determine_bar_lengths(data):
    """
    Determines bar length (duration in ms) for each bar timestamp in the transcript.
    Returns a dictionary mapping bar_key (str) -> bar_length_ms (float).
    """
    # Gather all unique bar timestamps across all tracks
    all_ts = set()
    bar_absolutes_diff = {}

    for track_id, track_data in data.items():
        if not isinstance(track_data, dict):
            continue
        for bar_key, bar_dict in track_data.items():
            try:
                ts = float(bar_key)
                all_ts.add(ts)
            except ValueError:
                pass

            if isinstance(bar_dict, dict) and "absolutes" in bar_dict:
                abss = bar_dict["absolutes"]
                if isinstance(abss, list) and len(abss) >= 2:
                    # step between consecutive notes
                    diffs = [abss[i+1] - abss[i] for i in range(len(abss)-1)]
                    if diffs and all(d > 0 for d in diffs):
                        step = diffs[0]
                        # Estimated bar length = (last note - first note) + step
                        est_len = (abss[-1] - abss[0]) + step
                        bar_absolutes_diff[bar_key] = est_len

    sorted_ts = sorted(list(all_ts))

    # Calculate differences between consecutive sorted bar timestamps
    ts_diffs = []
    if len(sorted_ts) > 1:
        for i in range(len(sorted_ts) - 1):
            d = sorted_ts[i+1] - sorted_ts[i]
            if d > 0:
                ts_diffs.append(d)

    default_bar_len = 2000.0
    if ts_diffs:
        # Use min or most common difference
        default_bar_len = min(ts_diffs)

    bar_lengths = {}
    for track_id, track_data in data.items():
        if not isinstance(track_data, dict):
            continue
        for bar_key, bar_dict in track_data.items():
            if bar_key in bar_absolutes_diff:
                bar_lengths[bar_key] = bar_absolutes_diff[bar_key]
            else:
                try:
                    ts = float(bar_key)
                    # Find next ts in sorted_ts
                    idx = sorted_ts.index(ts) if ts in sorted_ts else -1
                    if idx >= 0 and idx + 1 < len(sorted_ts):
                        bar_lengths[bar_key] = sorted_ts[idx + 1] - ts
                    else:
                        bar_lengths[bar_key] = default_bar_len
                except ValueError:
                    bar_lengths[bar_key] = default_bar_len

    return bar_lengths


def is_audio_silent(raw_bytes, sampwidth, comptype, threshold=1e-4):
    """
    Checks if raw PCM audio bytes are silent.
    Returns True if silent, False if contains audio.
    """
    if not raw_bytes:
        return True

    # Quick check for pure digital zero bytes
    if all(b == 0 for b in raw_bytes):
        return True

    num_bytes = len(raw_bytes)

    try:
        if sampwidth == 1:
            # 8-bit unsigned PCM (silence is 128)
            samples = [abs(b - 128) for b in raw_bytes]
            return max(samples) <= int(threshold * 128)
        elif sampwidth == 2:
            # 16-bit signed PCM
            num_samples = num_bytes // 2
            samples = struct.unpack(f'<{num_samples}h', raw_bytes)
            max_val = max(abs(s) for s in samples)
            return max_val <= int(threshold * 32768)
        elif sampwidth == 3:
            # 24-bit signed PCM
            max_val = 0
            for i in range(0, num_bytes, 3):
                val = int.from_bytes(raw_bytes[i:i+3], 'little', signed=True)
                if abs(val) > max_val:
                    max_val = abs(val)
            return max_val <= int(threshold * 8388608)
        elif sampwidth == 4:
            if comptype == 'FLOAT':
                num_samples = num_bytes // 4
                samples = struct.unpack(f'<{num_samples}f', raw_bytes)
                return max(abs(s) for s in samples) <= threshold
            else:
                num_samples = num_bytes // 4
                samples = struct.unpack(f'<{num_samples}i', raw_bytes)
                return max(abs(s) for s in samples) <= int(threshold * 2147483648)
    except Exception:
        pass

    return False


def check_and_remove_silent_bars(script_dir):
    # Locate transcript.json 3 levels up
    transcript_path = script_dir.parent.parent.parent / "transcript.json"

    print(f"Script location: {script_dir}")
    print(f"Looking for transcript.json at: {transcript_path}")

    if not transcript_path.exists():
        print(f"Error: 'transcript.json' not found at {transcript_path}")
        return

    try:
        with open(transcript_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error reading {transcript_path}: {e}")
        return

    if not isinstance(data, dict):
        print("Error: 'transcript.json' does not contain a valid JSON dictionary.")
        return

    most_negative_bar = get_most_negative_bar(data)
    bar_lengths = determine_bar_lengths(data)

    print(f"Loaded 'transcript.json' successfully.")
    print(f"Most negative bar: {most_negative_bar:.1f} ms ({format_time_str(most_negative_bar)})")

    # Scan WAV files in script directory starting with '01', '02', '03', '04'
    wav_files = [
        f for f in script_dir.iterdir()
        if f.is_file() and f.suffix.lower() == '.wav'
    ]

    prefix_to_track = {
        "01": "1",
        "02": "2",
        "03": "3",
        "04": "4"
    }

    matched_wavs = []
    for wf in wav_files:
        stem = wf.name
        for prefix, track_id in prefix_to_track.items():
            if stem.startswith(prefix):
                matched_wavs.append((wf, prefix, track_id))
                break

    if not matched_wavs:
        print("No WAV files starting with prefixes 01, 02, 03, or 04 found in script directory.")
        return

    print(f"Found {len(matched_wavs)} matching WAV file(s) to scan:\n")

    total_removed_bars = 0
    transcript_modified = False

    for wav_path, prefix, track_id in sorted(matched_wavs, key=lambda x: x[0].name):
        print(f"Scanning WAV file: '{wav_path.name}' (Track {track_id})")

        if track_id not in data or not isinstance(data[track_id], dict):
            print(f"  Track {track_id} not found in transcript.json. Skipping.\n")
            continue

        try:
            with wave.open(str(wav_path), 'rb') as wf:
                nchannels = wf.getnchannels()
                sampwidth = wf.getsampwidth()
                framerate = wf.getframerate()
                nframes = wf.getnframes()
                comptype = wf.getcomptype()

                wav_duration_sec = nframes / float(framerate) if framerate > 0 else 0.0
                wav_duration_ms = wav_duration_sec * 1000.0

                track_dict = data[track_id]
                bar_keys = list(track_dict.keys())

                bars_to_remove = []

                for bar_key in bar_keys:
                    try:
                        bar_ts = float(bar_key)
                    except ValueError:
                        continue

                    bar_len_ms = bar_lengths.get(bar_key, 2000.0)

                    # WAV file audio is 0-indexed: WAV start time = bar_ts - most_negative_bar
                    wav_start_ms = bar_ts - most_negative_bar
                    wav_end_ms = wav_start_ms + bar_len_ms

                    wav_start_sec = wav_start_ms / 1000.0
                    wav_end_sec = wav_end_ms / 1000.0

                    # Check if section falls within extent of WAV file
                    if wav_start_ms >= wav_duration_ms:
                        # Beyond WAV duration, skip or treat as silent?
                        # "for the extent of each WAV file, identify bar length sections of it corresponding to all the bars in the transcript dictionary"
                        continue

                    start_frame = max(0, int((wav_start_ms / 1000.0) * framerate))
                    end_frame = min(nframes, int((wav_end_ms / 1000.0) * framerate))

                    if start_frame >= end_frame:
                        continue

                    frames_to_read = end_frame - start_frame
                    wf.setpos(start_frame)
                    raw_bytes = wf.readframes(frames_to_read)

                    if is_audio_silent(raw_bytes, sampwidth, comptype):
                        bars_to_remove.append((bar_key, bar_ts, bar_len_ms, wav_start_sec, wav_end_sec, wav_start_ms, wav_end_ms))

                for bar_key, bar_ts, bar_len_ms, wav_start_sec, wav_end_sec, wav_start_ms, wav_end_ms in bars_to_remove:
                    if bar_key in track_dict:
                        del track_dict[bar_key]
                        total_removed_bars += 1
                        transcript_modified = True

                        abs_wav_str = f"{wav_start_sec:.2f}s - {wav_end_sec:.2f}s ({wav_start_ms:.1f}ms - {wav_end_ms:.1f}ms)"
                        ts_str = f"{bar_ts:.1f}ms ({format_time_str(bar_ts)})"

                        print(f"  [REMOVED SILENT BAR] Track {track_id} | Bar Key: '{bar_key}'")
                        print(f"    -> Absolute WAV Song Time : {abs_wav_str}")
                        print(f"    -> Transcript Timestamp   : {ts_str}")

                if not bars_to_remove:
                    print("  No silent bars found in this WAV file.")

        except Exception as e:
            print(f"  Error reading WAV file '{wav_path.name}': {e}")

        print()

    if transcript_modified:
        try:
            tmp_path = transcript_path.with_suffix('.json.tmp')
            with open(tmp_path, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2)
            os.replace(tmp_path, transcript_path)
            print(f"Successfully updated '{transcript_path.name}': removed {total_removed_bars} silent bar(s).")
        except Exception as e:
            print(f"Error saving updated '{transcript_path.name}': {e}")
    else:
        print("Transcript check complete: no silent bars were found or removed.")


def main():
    script_dir = Path(__file__).resolve().parent
    try:
        check_and_remove_silent_bars(script_dir)
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        import traceback
        traceback.print_exc()

    print("\nExecution complete.")
    input("Press Enter to exit...")


if __name__ == '__main__':
    main()

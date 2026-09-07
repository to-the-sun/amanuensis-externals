#!/usr/bin/env python3
"""
remove_silent_bars.py

Looks for transcript.json three folder levels up from the script's directory.
Scans all WAV files in the script's directory starting with prefixes '01', '02', '03', and '04',
correlating them with tracks 1, 2, 3, and 4 in transcript.json.

Scans all bar timestamps in transcript.json to determine the most common bar length among timestamp differences.
Warns if any bar timestamp is not an exact multiple of this bar length (or zero).

For each WAV file, identifies bar-length sections corresponding to all bars in the transcript dictionary.
Takes into account that transcript bar timestamps can be negative while audio in WAV files is 0-indexed:
  WAV_start_ms = bar_timestamp_ms - most_negative_bar_ms

If a bar-length audio section in a WAV file is silent, removes the associated bar from transcript.json
and prints an informative message stating both the absolute WAV file song time and the transcript timestamp.

Keeps the console open upon completion.
"""

import os
import sys
import wave
import struct
import json
from collections import Counter
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


def extract_bar_timestamp(bar_key, bar_dict):
    """
    Extracts the millisecond timestamp for a bar.
    Prioritizes explicit 'span' or 'absolutes' list data, falling back to float(bar_key).
    Returns float or None.
    """
    if isinstance(bar_dict, dict):
        if "span" in bar_dict and isinstance(bar_dict["span"], (list, tuple)) and len(bar_dict["span"]) > 0:
            try:
                return float(bar_dict["span"][0])
            except (ValueError, TypeError):
                pass

        if "absolutes" in bar_dict and isinstance(bar_dict["absolutes"], (list, tuple)) and len(bar_dict["absolutes"]) > 0:
            try:
                return float(bar_dict["absolutes"][0])
            except (ValueError, TypeError):
                pass

    try:
        return float(bar_key)
    except (ValueError, TypeError):
        return None


def get_most_negative_bar(data):
    """
    Finds the lowest (most negative) bar millisecond timestamp across all tracks in the transcript.
    Returns 0.0 if no negative bar timestamps exist.
    """
    most_neg = 0.0
    for track_id, track_data in data.items():
        if not isinstance(track_data, dict):
            continue
        for bar_key, bar_dict in track_data.items():
            ts = extract_bar_timestamp(bar_key, bar_dict)
            if ts is not None and ts < most_neg:
                most_neg = ts
    return most_neg


def determine_bar_length_and_check_multiples(data):
    """
    Scans transcript.json bar timestamps, finds the most common difference between adjacent bar timestamps,
    and returns it as the global bar length (in ms).
    Warns if any bar timestamp is not a multiple of this bar length (or zero).
    """
    all_bars = []
    track_diffs = []

    for track_id, track_data in data.items():
        if not isinstance(track_data, dict):
            continue

        track_ts_list = []
        for bar_key, bar_dict in track_data.items():
            ts = extract_bar_timestamp(bar_key, bar_dict)
            if ts is not None:
                all_bars.append((track_id, bar_key, ts))
                track_ts_list.append(ts)

        sorted_track_ts = sorted(list(set(track_ts_list)))
        if len(sorted_track_ts) > 1:
            for i in range(len(sorted_track_ts) - 1):
                diff = sorted_track_ts[i+1] - sorted_track_ts[i]
                if diff > 0:
                    track_diffs.append(round(diff, 2))

    # Also check differences across all combined unique timestamps
    combined_ts = sorted(list(set(b[2] for b in all_bars)))
    combined_diffs = []
    if len(combined_ts) > 1:
        for i in range(len(combined_ts) - 1):
            diff = combined_ts[i+1] - combined_ts[i]
            if diff > 0:
                combined_diffs.append(round(diff, 2))

    all_diffs = track_diffs if track_diffs else combined_diffs

    default_bar_len = 2000.0
    if all_diffs:
        counts = Counter(all_diffs)
        most_common_bar_len = float(counts.most_common(1)[0][0])
    else:
        most_common_bar_len = default_bar_len

    if most_common_bar_len <= 0:
        most_common_bar_len = default_bar_len

    print(f"Detected primary bar length: {most_common_bar_len:.1f} ms")

    # Check for any bar timestamp that is not a multiple of most_common_bar_len (or zero)
    warnings_found = False
    for track_id, bar_key, ts in all_bars:
        if ts == 0.0:
            continue

        rem = abs(ts) % most_common_bar_len
        is_multiple = (rem < 1e-2) or (abs(rem - most_common_bar_len) < 1e-2)

        if not is_multiple:
            warnings_found = True
            print(f"  [WARNING] Track {track_id} | Bar Key '{bar_key}' timestamp ({ts:.1f} ms) is not a multiple of detected bar length ({most_common_bar_len:.1f} ms).")

    if not warnings_found and all_bars:
        print("  All bar timestamps are exact multiples of the detected bar length (or zero).")

    print()
    return most_common_bar_len


def is_audio_silent(raw_bytes, sampwidth, comptype, threshold=1e-4):
    """
    Checks if raw PCM audio bytes are silent.
    Returns True if silent, False if contains audio.
    """
    if not raw_bytes:
        return True

    num_bytes = len(raw_bytes)

    try:
        if sampwidth == 1:
            # 8-bit unsigned PCM (silence is byte value 128)
            samples = [abs(b - 128) for b in raw_bytes]
            return max(samples) <= int(threshold * 128)
        elif sampwidth == 2:
            # 16-bit signed PCM
            num_samples = num_bytes // 2
            samples = struct.unpack(f'<{num_samples}h', raw_bytes)
            return max(abs(s) for s in samples) <= int(threshold * 32768)
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

    most_negative_bar_ms = get_most_negative_bar(data)
    print(f"Loaded 'transcript.json' successfully.")
    print(f"Most negative bar: {most_negative_bar_ms:.1f} ms ({format_time_str(most_negative_bar_ms)})\n")

    bar_len_ms = determine_bar_length_and_check_multiples(data)

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
                    bar_dict = track_dict[bar_key]
                    bar_ts_ms = extract_bar_timestamp(bar_key, bar_dict)

                    if bar_ts_ms is None:
                        continue

                    # WAV file audio is 0-indexed: WAV start time = bar_ts_ms - most_negative_bar_ms
                    wav_start_ms = bar_ts_ms - most_negative_bar_ms
                    wav_end_ms = wav_start_ms + bar_len_ms

                    wav_start_sec = wav_start_ms / 1000.0
                    wav_end_sec = wav_end_ms / 1000.0

                    # Check if section falls within extent of WAV file
                    if wav_start_ms >= wav_duration_ms:
                        continue

                    start_frame = max(0, int((wav_start_ms / 1000.0) * framerate))
                    end_frame = min(nframes, int((wav_end_ms / 1000.0) * framerate))

                    if start_frame >= end_frame:
                        continue

                    frames_to_read = end_frame - start_frame
                    wf.setpos(start_frame)
                    raw_bytes = wf.readframes(frames_to_read)

                    if is_audio_silent(raw_bytes, sampwidth, comptype):
                        bars_to_remove.append((bar_key, bar_ts_ms, bar_len_ms, wav_start_sec, wav_end_sec, wav_start_ms, wav_end_ms))

                for bar_key, bar_ts_ms, bar_len_ms, wav_start_sec, wav_end_sec, wav_start_ms, wav_end_ms in bars_to_remove:
                    if bar_key in track_dict:
                        del track_dict[bar_key]
                        total_removed_bars += 1
                        transcript_modified = True

                        abs_wav_str = f"{wav_start_sec:.2f}s - {wav_end_sec:.2f}s ({wav_start_ms:.1f}ms - {wav_end_ms:.1f}ms)"
                        ts_str = f"{bar_ts_ms:.1f}ms ({format_time_str(bar_ts_ms)})"

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

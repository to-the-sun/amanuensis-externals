#!/usr/bin/env python3
"""
truncate.py

Truncates all WAV files in the same directory as this script after the 6:30 mark (390.0 seconds).
Applies a smooth linear fade-out over the final 2 seconds before the 6:30 cutoff so audio ends cleanly.
Also checks for a transcript.json file in the same directory and removes any bars occurring after 6:30 (390,000 ms)
of total song time, taking into account any negative bars (most_negative_bar).
"""

import os
import sys
import wave
import struct
import json
import re
import shutil
from datetime import datetime
from pathlib import Path

CUTOFF_SECONDS = 390.0  # 6 minutes 30 seconds
CUTOFF_MS = CUTOFF_SECONDS * 1000.0  # 390,000 ms
FADE_SECONDS = 2.0  # 2 second fade out before cutoff


def format_ms_time(ms):
    """
    Formats a millisecond value into a readable string like '-1:14' or '5:16'.
    """
    is_neg = ms < 0
    total_sec = abs(ms) / 1000.0
    minutes = int(total_sec // 60)
    seconds = total_sec % 60
    sign = "-" if is_neg else ""
    if seconds == int(seconds):
        return f"{sign}{minutes}:{int(seconds):02d}"
    return f"{sign}{minutes}:{seconds:04.1f}"


def truncate_wav(filepath, cutoff_sec=CUTOFF_SECONDS, fade_sec=FADE_SECONDS):
    """
    Truncates a single WAV file to cutoff_sec if its duration exceeds cutoff_sec.
    Applies a clean linear fade-out over fade_sec before cutoff_sec.
    """
    try:
        with wave.open(filepath, 'rb') as wf:
            nchannels = wf.getnchannels()
            sampwidth = wf.getsampwidth()
            framerate = wf.getframerate()
            nframes = wf.getnframes()
            comptype = wf.getcomptype()
            compname = wf.getcompname()

            duration = nframes / float(framerate)
            if duration <= cutoff_sec:
                print(f"Skipping '{os.path.basename(filepath)}': duration ({duration:.2f}s) is already <= {cutoff_sec:.1f}s.")
                return False

            target_frames = int(cutoff_sec * framerate)
            fade_frames = int(fade_sec * framerate)
            fade_frames = min(fade_frames, target_frames)
            fade_start_frame = target_frames - fade_frames

            raw_bytes = wf.readframes(target_frames)

        bytes_per_frame = nchannels * sampwidth
        unmodified_bytes_len = fade_start_frame * bytes_per_frame
        unmodified_data = raw_bytes[:unmodified_bytes_len]
        fade_data_raw = raw_bytes[unmodified_bytes_len:]

        faded_bytes = bytearray()
        num_fade_frames = target_frames - fade_start_frame

        for f_idx in range(num_fade_frames):
            gain = 1.0 - (f_idx / float(num_fade_frames))
            frame_offset = f_idx * bytes_per_frame

            for ch in range(nchannels):
                sample_offset = frame_offset + ch * sampwidth
                sample_bytes = fade_data_raw[sample_offset : sample_offset + sampwidth]

                if sampwidth == 1:
                    val = sample_bytes[0]
                    faded_val = 128 + int((val - 128) * gain)
                    faded_val = max(0, min(255, faded_val))
                    faded_bytes.append(faded_val)
                elif sampwidth == 2:
                    val = struct.unpack('<h', sample_bytes)[0]
                    faded_val = int(val * gain)
                    faded_val = max(-32768, min(32767, faded_val))
                    faded_bytes.extend(struct.pack('<h', faded_val))
                elif sampwidth == 3:
                    val = int.from_bytes(sample_bytes, 'little', signed=True)
                    faded_val = int(val * gain)
                    faded_val = max(-8388608, min(8388607, faded_val))
                    faded_bytes.extend(faded_val.to_bytes(3, 'little', signed=True))
                elif sampwidth == 4:
                    if comptype == 'FLOAT':
                        val = struct.unpack('<f', sample_bytes)[0]
                        faded_val = val * gain
                        faded_bytes.extend(struct.pack('<f', faded_val))
                    else:
                        val = struct.unpack('<i', sample_bytes)[0]
                        faded_val = int(val * gain)
                        faded_val = max(-2147483648, min(2147483647, faded_val))
                        faded_bytes.extend(struct.pack('<i', faded_val))

        final_data = unmodified_data + bytes(faded_bytes)

        tmp_filepath = filepath + ".tmp"
        with wave.open(tmp_filepath, 'wb') as out_wf:
            out_wf.setnchannels(nchannels)
            out_wf.setsampwidth(sampwidth)
            out_wf.setframerate(framerate)
            out_wf.setcomptype(comptype, compname)
            out_wf.writeframes(final_data)

        os.replace(tmp_filepath, filepath)
        print(f"Truncated '{os.path.basename(filepath)}' from {duration:.2f}s to {cutoff_sec:.1f}s with a {fade_sec:.1f}s clean fade out.")
        return True

    except Exception as e:
        print(f"Error processing WAV file '{os.path.basename(filepath)}': {e}")
        return False


def locate_transcript(script_dir):
    """
    Locates transcript.json:
      1. In the same directory as the script.
      2. If not found, 3 folder levels up from the script's directory.
    Returns transcript_path or None.
    """
    local_path = script_dir / "transcript.json"
    if local_path.is_file():
        return local_path

    three_up_path = script_dir.parent.parent.parent / "transcript.json"
    if three_up_path.is_file():
        return three_up_path

    return None


def save_formatted_json(data, filepath):
    """
    Saves JSON data with indent=2 formatting, but with inline single-line array formatting.
    """
    filepath = Path(filepath)
    raw_str = json.dumps(data, indent=2)

    def collapse_array(match):
        inner = match.group(1)
        items = [line.strip().rstrip(',') for line in inner.splitlines() if line.strip()]
        return '[' + ', '.join(items) + ']'

    formatted_str = re.sub(r'\[\s*([^\[\]\{\}]*?)\s*\]', collapse_array, raw_str)

    tmp_path = filepath.with_suffix('.json.tmp')
    with open(tmp_path, 'w', encoding='utf-8') as f:
        f.write(formatted_str)
        f.write('\n')
    os.replace(tmp_path, filepath)


def process_transcript(json_filepath, cutoff_duration_ms=CUTOFF_MS):
    """
    Removes every bar from transcript.json that occurs after cutoff_duration_ms (390,000 ms = 6:30)
    of absolute song time, taking into account negative bars (most_negative_bar).
    """
    if json_filepath is None or not os.path.exists(json_filepath):
        print(f"No 'transcript.json' found.")
        return False

    try:
        with open(json_filepath, 'r', encoding='utf-8') as f:
            data = json.load(f)

        if not isinstance(data, dict):
            print("Error: 'transcript.json' does not contain a JSON dictionary.")
            return False

        # Find most_negative_bar across all tracks
        most_negative_bar = 0.0
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

                if bar_ts is not None and bar_ts < most_negative_bar:
                    most_negative_bar = bar_ts

        cutoff_bar_ts = cutoff_duration_ms + most_negative_bar
        most_neg_str = format_ms_time(most_negative_bar)
        cutoff_str = format_ms_time(cutoff_bar_ts)

        print(f"'transcript.json' analysis: most_negative_bar = {most_negative_bar:.1f} ms ({most_neg_str}) -> cutoff bar timestamp = {cutoff_bar_ts:.1f} ms ({cutoff_str}).")

        removed_bars_count = 0
        modified = False

        for track_id, track_data in list(data.items()):
            if not isinstance(track_data, dict):
                continue

            bars_to_remove = []
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

                if bar_ts is not None and bar_ts > cutoff_bar_ts:
                    bars_to_remove.append(bar_key)

            for bar_key in bars_to_remove:
                del track_data[bar_key]
                removed_bars_count += 1
                modified = True

            # Clean up span lists in remaining bars
            for bar_key, bar_dict in track_data.items():
                if isinstance(bar_dict, dict) and "span" in bar_dict and isinstance(bar_dict["span"], list):
                    orig_len = len(bar_dict["span"])
                    bar_dict["span"] = [s for s in bar_dict["span"] if float(s) <= cutoff_bar_ts]
                    if len(bar_dict["span"]) != orig_len:
                        modified = True

        if modified:
            # Check for Backup/ directory in the transcript's parent folder
            tx_path = Path(json_filepath)
            backup_dir = tx_path.parent / "Backup"
            if backup_dir.is_dir():
                timestamp_str = datetime.now().strftime("%Y-%m-%d %H%M%S")
                backup_filename = f"transcript [{timestamp_str}].json"
                backup_filepath = backup_dir / backup_filename
                try:
                    shutil.copy2(tx_path, backup_filepath)
                    print(f"Created backup at: {backup_filepath}")
                except Exception as e:
                    print(f"Warning: Failed to create backup in '{backup_dir}': {e}")

            save_formatted_json(data, json_filepath)
            print(f"Updated '{os.path.basename(json_filepath)}': removed {removed_bars_count} bar(s) occurring after timestamp {cutoff_bar_ts:.1f} ms ({cutoff_str}, 6:30 absolute song time).")
        else:
            print(f"'{os.path.basename(json_filepath)}' checked: no bars occurred after timestamp {cutoff_bar_ts:.1f} ms ({cutoff_str}).")

        return modified

    except Exception as e:
        print(f"Error processing 'transcript.json': {e}")
        return False


def main():
    try:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        print(f"Running truncation in: {script_dir}")
        print(f"Target duration: {CUTOFF_SECONDS:.1f}s (6:30) with {FADE_SECONDS:.1f}s fade out.\n")

        # 1. Process all WAV files in script directory
        wav_files = [
            f for f in os.listdir(script_dir)
            if f.lower().endswith('.wav') and not f.endswith('.tmp') and os.path.isfile(os.path.join(script_dir, f))
        ]

        if not wav_files:
            print("No WAV files found in the script directory.")
        else:
            print(f"Found {len(wav_files)} WAV file(s). Processing...")
            for wav_file in sorted(wav_files):
                wav_path = os.path.join(script_dir, wav_file)
                truncate_wav(wav_path)

        print()

        # 2. Process transcript.json
        script_dir_path = Path(script_dir)
        json_path = locate_transcript(script_dir_path)
        if json_path is None:
            local_check = script_dir_path / "transcript.json"
            three_up_check = script_dir_path.parent.parent.parent / "transcript.json"
            print(f"No 'transcript.json' found. Checked:\n  1. {local_check}\n  2. {three_up_check}")
        else:
            print(f"Found 'transcript.json' at: {json_path}")
            process_transcript(json_path)

        print("\nProcessing complete.")
    except Exception as e:
        print(f"\nAn error occurred during execution: {e}")
        import traceback
        traceback.print_exc()

    input("\nPress Enter to exit...")


if __name__ == '__main__':
    main()

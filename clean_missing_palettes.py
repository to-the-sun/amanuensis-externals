#!/usr/bin/env python3
"""
clean_missing_palettes.py

Looks for transcript.json in the same folder as the script, or if not found there,
looks three folder levels up from the script's directory.

Scans transcript.json to identify all palette WAV files referenced across all tracks and bars.
Compares them against the WAV files located in the same directory as transcript.json (including checking
for files with the 'palette_' prefix and '.wav' extension).

For any bar referencing a palette WAV file that does not exist in that directory,
removes that bar from transcript.json and prints an informative message to the console.

Keeps the console open upon completion.
"""

import os
import sys
import json
from pathlib import Path


def locate_transcript(script_dir):
    """
    Locates transcript.json:
      1. In the same directory as the script.
      2. If not found, 3 folder levels up from the script's directory.
    Returns (transcript_path, transcript_dir) or (None, None).
    """
    local_path = script_dir / "transcript.json"
    if local_path.is_file():
        return local_path, script_dir

    three_up_path = script_dir.parent.parent.parent / "transcript.json"
    if three_up_path.is_file():
        return three_up_path, three_up_path.parent

    return None, None


def clean_missing_palettes():
    script_dir = Path(__file__).resolve().parent
    transcript_path, transcript_dir = locate_transcript(script_dir)

    print(f"Script location: {script_dir}")

    if transcript_path is None:
        local_check = script_dir / "transcript.json"
        three_up_check = script_dir.parent.parent.parent / "transcript.json"
        print(f"Error: Could not find 'transcript.json'. Checked:")
        print(f"  1. {local_check}")
        print(f"  2. {three_up_check}")
        return

    print(f"Found 'transcript.json' at: {transcript_path}")
    print(f"Checking palette WAV files in directory: {transcript_dir}\n")

    try:
        with open(transcript_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error reading '{transcript_path}': {e}")
        return

    if not isinstance(data, dict):
        print("Error: 'transcript.json' does not contain a valid JSON dictionary.")
        return

    # Gather set of existing filenames in transcript_dir
    existing_files = {f.name for f in transcript_dir.iterdir() if f.is_file()}

    total_removed_bars = 0
    transcript_modified = False

    # Process all tracks in transcript.json
    for track_id, track_data in list(data.items()):
        if not isinstance(track_data, dict):
            continue

        bars_to_remove = []

        for bar_key, bar_dict in list(track_data.items()):
            if not isinstance(bar_dict, dict):
                continue

            palette_val = bar_dict.get("palette")
            if not palette_val:
                continue

            if isinstance(palette_val, (list, tuple)):
                if len(palette_val) == 0:
                    continue
                palette_name = str(palette_val[0]).strip()
            else:
                palette_name = str(palette_val).strip()

            if not palette_name:
                continue

            # Derive candidate file names to search for on disk (adding 'palette_' prefix and '.wav' if needed)
            wav_name = palette_name if palette_name.lower().endswith(".wav") else f"{palette_name}.wav"
            prefixed_name = wav_name if wav_name.lower().startswith("palette_") else f"palette_{wav_name}"

            candidate_names = {
                palette_name,
                wav_name,
                prefixed_name
            }

            file_exists = any(
                (transcript_dir / c).is_file() or c in existing_files
                for c in candidate_names
            )

            if not file_exists:
                bars_to_remove.append((bar_key, palette_name))

        for bar_key, palette_name in bars_to_remove:
            del track_data[bar_key]
            total_removed_bars += 1
            transcript_modified = True

            print(f"  [REMOVED MISSING PALETTE BAR]")
            print(f"    -> Track ID        : {track_id}")
            print(f"    -> Bar Key         : '{bar_key}'")
            print(f"    -> Missing Palette : '{palette_name}'")

    print()

    if transcript_modified:
        try:
            tmp_path = transcript_path.with_suffix('.json.tmp')
            with open(tmp_path, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=2)
            os.replace(tmp_path, transcript_path)
            print(f"Successfully updated '{transcript_path.name}': removed {total_removed_bars} bar(s) with missing palettes.")
        except Exception as e:
            print(f"Error saving updated '{transcript_path.name}': {e}")
    else:
        print("Transcript check complete: all referenced palette WAV files were found in the directory.")


def main():
    try:
        clean_missing_palettes()
    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        import traceback
        traceback.print_exc()

    print("\nExecution complete.")
    input("Press Enter to exit...")


if __name__ == '__main__':
    main()

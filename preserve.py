import os
import re
import sys
import json
from datetime import datetime
from pathlib import Path
import soundfile as sf
import numpy as np

def locate_transcript():
    """
    Locates transcript.json three directory levels up from where the script is executed,
    or three levels up from the script's directory.
    """
    cwd_three_up = Path.cwd().parent.parent.parent / "transcript.json"
    if cwd_three_up.is_file():
        return cwd_three_up

    script_three_up = Path(__file__).resolve().parent.parent.parent / "transcript.json"
    if script_three_up.is_file():
        return script_three_up

    return cwd_three_up


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


def remove_track_from_transcript(track_num):
    """
    Removes the specified track entry from transcript.json located three levels up.
    """
    transcript_path = locate_transcript()
    if not transcript_path.is_file():
        print(f"transcript.json not found at expected location: {transcript_path}")
        return False

    try:
        with open(transcript_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error reading '{transcript_path}': {e}")
        return False

    if not isinstance(data, dict):
        print(f"Error: '{transcript_path}' does not contain a valid JSON dictionary.")
        return False

    track_str = str(track_num)
    track_str_padded = f"{track_num:02d}"

    removed = False
    keys_to_remove = [k for k in data.keys() if k == track_str or k == track_str_padded]

    for k in keys_to_remove:
        del data[k]
        removed = True
        print(f"Removed track '{k}' from {transcript_path}")

    if removed:
        try:
            save_formatted_json(data, transcript_path)
            print(f"Successfully updated '{transcript_path.name}'.")
            return True
        except Exception as e:
            print(f"Error saving updated '{transcript_path.name}': {e}")
            return False
    else:
        print(f"Track '{track_str}' was not found in {transcript_path}.")
        return False


def get_next_prefix(directory):
    max_prefix = 0
    try:
        filenames = os.listdir(directory)
    except OSError:
        filenames = []

    for filename in filenames:
        match = re.match(r'^(\d{2})(?!\d)', filename)
        if match:
            val = int(match.group(1))
            if val > max_prefix:
                max_prefix = val
    return f"{max_prefix + 1:02d}"

def process_file(filepath):
    print(f"\nProcessing: {filepath}")
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return

    abs_filepath = os.path.abspath(filepath)
    directory = os.path.dirname(abs_filepath)
    filename = os.path.basename(abs_filepath)
    name, ext = os.path.splitext(filename)

    try:
        # Read the original file
        data, samplerate = sf.read(abs_filepath)
        info = sf.info(abs_filepath)
        print(f"Read successful: {info.channels} channels, {info.frames} frames, {samplerate}Hz, format: {info.format}")

        now = datetime.now()
        timestamp_str = now.strftime("%Y-%m-%d %H%M%S")
        ts_pattern = r'\[\d{4}-\d{1,2}-\d{1,2}(?: \d{6}|-\d{1,2}-\d{1,2}-\d{1,2})\]'

        # 1. Handle original (record-keeping silenced version)
        match = re.match(r'^(\d{1,2})(?!\d)', filename)
        should_silence = False
        if match:
            original_num = int(match.group(1))
            print(f"Found prefix: {original_num}")
            if 1 <= original_num <= 4:
                print(f"Prefix {original_num} <= 4, will create silenced record and remove track from transcript.json.")
                should_silence = True
                remove_track_from_transcript(original_num)
        else:
            print("No numbered prefix found. Skipping silenced record creation.")

        if should_silence:
            # Update timestamp on record name
            if re.search(ts_pattern, name):
                record_name = re.sub(ts_pattern, f"[{timestamp_str}]", name)
            else:
                record_name = f"{name} [{timestamp_str}]"

            record_path = os.path.join(directory, record_name + ".wav")

            # Create silent data
            silent_data = np.zeros_like(data)

            print(f"Writing silenced record: {record_name}.wav")
            sf.write(record_path, silent_data, samplerate, subtype=info.subtype)

        # 2. Create the preserved copy (contains audio)
        next_prefix = get_next_prefix(directory)
        print(f"Calculated next prefix: {next_prefix}")

        copy_name = name
        if re.match(r'^\d{2}(?!\d)', name):
            copy_name = re.sub(r'^\d{2}', next_prefix, name, count=1)
        else:
            copy_name = f"{next_prefix} {name}"

        if not re.search(ts_pattern, copy_name):
            copy_name = f"{copy_name} [{timestamp_str}]"

        # Force .wav extension for all output files
        new_filepath = os.path.join(directory, copy_name + ".wav")

        print(f"Writing preserved copy (with audio): {copy_name}.wav")
        sf.write(new_filepath, data, samplerate, subtype=info.subtype)

        # 3. Cleanup original
        os.remove(abs_filepath)
        print(f"Original file removed: {filename}")
        return True

    except Exception as e:
        print(f"Error processing {filepath}: {e}")
        import traceback
        traceback.print_exc()
        return False

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 preserve.py <file1> <file2> ...")
        sys.exit(1)

    for arg in sys.argv[1:]:
        process_file(arg)

if __name__ == "__main__":
    main()

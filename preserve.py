import os
import re
import sys
import shutil
from datetime import datetime
import soundfile as sf
import numpy as np

def get_next_prefix(directory):
    max_prefix = -1
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

def silence_audio_file(filepath):
    print(f"--- Silencing process for: {filepath}")
    try:
        # Read the audio file
        data, samplerate = sf.read(filepath)
        info = sf.info(filepath)
        print(f"Audio info: {info.channels} channels, {info.frames} frames, {samplerate}Hz, format: {info.format} ({info.subtype})")

        # Create silent data of the same shape and type
        silent_data = np.zeros_like(data)

        # Write back to the same file, preserving format and subtype
        sf.write(filepath, silent_data, samplerate, subtype=info.subtype)
        print("Silencing complete.")
        return True
    except Exception as e:
        print(f"Error silencing {filepath}: {e}")
        import traceback
        traceback.print_exc()
        return False

def process_file(filepath):
    print(f"\nProcessing: {filepath}")
    if not os.path.exists(filepath):
        print(f"File not found: {filepath}")
        return

    abs_filepath = os.path.abspath(filepath)
    directory = os.path.dirname(abs_filepath)
    filename = os.path.basename(abs_filepath)
    name, ext = os.path.splitext(filename)

    now = datetime.now()
    timestamp_str = now.strftime("%Y-%m-%d %H%M%S")
    ts_pattern = r'\[\d{4}-\d{1,2}-\d{1,2}(?: \d{6}|-\d{1,2}-\d{1,2}-\d{1,2})\]'

    # 1. Prefix for the copy
    next_prefix = get_next_prefix(directory)
    print(f"Calculated next prefix: {next_prefix}")

    copy_name = name
    if re.match(r'^\d{2}(?!\d)', name):
        copy_name = re.sub(r'^\d{2}', next_prefix, name, count=1)
    else:
        copy_name = f"{next_prefix} {name}"

    # 2. Timestamp for the copy
    if not re.search(ts_pattern, copy_name):
        copy_name = f"{copy_name} [{timestamp_str}]"

    new_filepath = os.path.join(directory, copy_name + ext)

    # Perform copy
    shutil.copy2(abs_filepath, new_filepath)
    print(f"Copied: {filename} -> {copy_name + ext}")

    # 3. Silencing and renaming original
    print(f"Checking if original file {filename} should be silenced...")
    match = re.match(r'^(\d{1,2})(?!\d)', filename)
    if match:
        original_num = int(match.group(1))
        print(f"Found prefix: {original_num}")
        if original_num <= 4:
            print(f"Prefix {original_num} <= 4, proceeding to silence original.")
            if silence_audio_file(abs_filepath):
                print(f"Silenced: {filename}")

                # Update timestamp on original file name
                if re.search(ts_pattern, name):
                    original_new_name = re.sub(ts_pattern, f"[{timestamp_str}]", name)
                else:
                    original_new_name = f"{name} [{timestamp_str}]"

                original_new_path = os.path.join(directory, original_new_name + ext)
                if abs_filepath != original_new_path:
                    os.rename(abs_filepath, original_new_path)
                    print(f"Renamed original: {filename} -> {original_new_name + ext}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 preserve.py <file1> <file2> ...")
        sys.exit(1)

    for arg in sys.argv[1:]:
        process_file(arg)

    print("\nProcessing complete.")
    input("Press Enter to close...")

if __name__ == "__main__":
    main()

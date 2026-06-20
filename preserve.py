import os
import re
import sys
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
            if original_num <= 4:
                print(f"Prefix {original_num} <= 4, will create silenced record.")
                should_silence = True
        else:
            print("No numbered prefix found. Treating as prefix <= 4, will create silenced record.")
            should_silence = True

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

    print("\nProcessing complete.")
    input("Press Enter to close...")

if __name__ == "__main__":
    main()

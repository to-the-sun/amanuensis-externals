import os
import sys
import shutil
import argparse
from datetime import datetime
import re

def get_modified_filename(filename):
    """
    Renames the filename:
    1. Identifies the base name by removing any trailing timestamp in brackets.
    2. Inserts ' (doubles) ' before the new timestamp.
    3. Adds/Updates the timestamp to [YYYY-MM-DD HHMMSS].
    """
    base, ext = os.path.splitext(filename)

    # Regex to find a trailing timestamp in square [] or angled <> brackets
    # It accounts for optional leading whitespace.
    timestamp_pattern = r'[\s]*[\[<][^\]>]*[\]>]$'
    match = re.search(timestamp_pattern, base)

    if match:
        # Extract the part before the timestamp
        part1 = base[:match.start()].rstrip()
    else:
        part1 = base

    # Generate new timestamp: [YYYY-MM-DD HHMMSS]
    new_timestamp = datetime.now().strftime("%Y-%m-%d %H%M%S")

    return f"{part1} (doubles) [{new_timestamp}]{ext}"

def main():
    parser = argparse.ArgumentParser(description="Copy and rename audio files with '(doubles)' and an updated timestamp.")
    parser.add_argument("files", nargs="+", help="Audio files to process")

    if len(sys.argv) == 1:
        parser.print_help()
        sys.exit(1)

    args = parser.parse_args()

    for filepath in args.files:
        if not os.path.isfile(filepath):
            print(f"Error: '{filepath}' is not a valid file.")
            continue

        dir_name = os.path.dirname(filepath)
        old_name = os.path.basename(filepath)
        new_name = get_modified_filename(old_name)

        # Use the same directory as the original
        new_path = os.path.join(dir_name, new_name)

        # Avoid overwriting if by some chance the name is identical (though unlikely with seconds)
        if os.path.exists(new_path) and os.path.samefile(filepath, new_path):
             print(f"Skip: Destination is same as source for '{old_name}'")
             continue

        try:
            shutil.copy2(filepath, new_path)
            print(f"Success: Copied and renamed '{old_name}' -> '{new_name}'")
        except Exception as e:
            print(f"Error: Failed to copy '{filepath}': {e}")

if __name__ == "__main__":
    main()

import os
import re
import sys
import shutil
import wave
import aifc
import struct
from datetime import datetime

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

def convert_to_wav(filepath):
    print(f"--- Converting to WAV: {filepath}")
    name, ext = os.path.splitext(filepath)
    if ext.lower() not in ['.aif', '.aiff']:
        print(f"Skipping conversion: {ext} is not AIFF")
        return filepath

    wav_filepath = name + ".wav"
    try:
        with aifc.open(filepath, 'rb') as aif_in:
            nchannels = aif_in.getnchannels()
            sampwidth = aif_in.getsampwidth()
            framerate = aif_in.getframerate()
            nframes = aif_in.getnframes()

            print(f"AIFF params: {nchannels} channels, {sampwidth} bytes/sample, {nframes} frames, {framerate}Hz")

            with wave.open(wav_filepath, 'wb') as wav_out:
                wav_out.setnchannels(nchannels)
                wav_out.setsampwidth(sampwidth)
                wav_out.setframerate(framerate)

                # Copy audio data in chunks and handle endianness/signing
                frames_left = nframes
                chunk_size = 1000 # smaller chunk for easier processing

                # Format string for struct
                if sampwidth == 1:
                    fmt_in = 'b' # signed char
                    fmt_out = 'B' # unsigned char
                elif sampwidth == 2:
                    fmt_in = '>h' # big-endian short
                    fmt_out = '<h' # little-endian short
                elif sampwidth == 3:
                    # 3-byte is tricky, we'll handle manually
                    fmt_in = None
                    fmt_out = None
                elif sampwidth == 4:
                    fmt_in = '>i' # big-endian int
                    fmt_out = '<i' # little-endian int
                else:
                    raise ValueError(f"Unsupported sample width: {sampwidth}")

                while frames_left > 0:
                    n = min(chunk_size, frames_left)
                    data = aif_in.readframes(n)

                    if sampwidth == 1:
                        # signed to unsigned (add 128)
                        values = struct.unpack(str(n * nchannels) + fmt_in, data)
                        converted_values = [v + 128 for v in values]
                        out_data = struct.pack(str(n * nchannels) + fmt_out, *converted_values)
                    elif sampwidth == 3:
                        # Handle 24-bit (3-byte) manually
                        out_data_list = []
                        for i in range(0, len(data), 3):
                            chunk = data[i:i+3]
                            # AIFF is Big Endian, WAV is Little Endian
                            out_data_list.append(chunk[::-1])
                        out_data = b''.join(out_data_list)
                    elif fmt_in and fmt_out:
                        # swap endianness
                        count = n * nchannels
                        values = struct.unpack(fmt_in[0] + fmt_in[1] * count, data)
                        out_data = struct.pack(fmt_out[0] + fmt_out[1] * count, *values)
                    else:
                        out_data = data # Fallback

                    wav_out.writeframes(out_data)
                    frames_left -= n

        print(f"Conversion successful: {wav_filepath}")
        os.remove(filepath)
        print(f"Original AIFF removed: {filepath}")
        return wav_filepath
    except Exception as e:
        print(f"Error converting {filepath} to WAV: {e}")
        import traceback
        traceback.print_exc()
        # If conversion failed, don't return the wav_filepath as it might be corrupted or incomplete
        if os.path.exists(wav_filepath):
            os.remove(wav_filepath)
        return filepath

def silence_audio_file(filepath):
    print(f"--- Silencing process for: {filepath}")
    ext = os.path.splitext(filepath)[1].lower()
    if ext != '.wav':
        print(f"Warning: {ext} is not WAV. Silencing might fail if not PCM.")

    try:
        with wave.open(filepath, 'rb') as audio_in:
            n_frames = audio_in.getnframes()
            sampwidth = audio_in.getsampwidth()
            nchannels = audio_in.getnchannels()
            framerate = audio_in.getframerate()
            print(f"Audio params: {nchannels} channels, {sampwidth} bytes/sample, {n_frames} frames, {framerate}Hz")

        print(f"Opening {filepath} for writing silence...")
        with wave.open(filepath, 'wb') as audio_out:
            audio_out.setnchannels(nchannels)
            audio_out.setsampwidth(sampwidth)
            audio_out.setframerate(framerate)

            if sampwidth == 1:
                # 8-bit unsigned PCM in WAV: 128 (0x80) is silence
                print("Using 0x80 (128) for 8-bit WAV silence")
                silence_data = b'\x80' * (n_frames * nchannels)
            else:
                # 16, 24, 32-bit signed PCM in WAV: 0 is silence
                print("Using 0x00 (0) for silence")
                silence_data = b'\x00' * (n_frames * nchannels * sampwidth)

            audio_out.writeframes(silence_data)
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

    # Convert to WAV if it's AIFF
    filepath = convert_to_wav(filepath)

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
    # "it should not actually update the timestamp... unless that timestamp is being added for the first time"
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

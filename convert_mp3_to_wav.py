#!/usr/bin/env python3
"""
convert_mp3_to_wav.py

Scans the directory where this script is located for any MP3 files (.mp3)
and automatically converts them to WAV format (.wav) at a 44.1 kHz (44,100 Hz) sample rate.
Also checks 3 folder levels up for 'description.txt' and creates it if missing.
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path


TARGET_SAMPLE_RATE = 44100  # 44.1 kHz


def ensure_description_txt(mp3_file, script_dir):
    """
    Looks 3 folder levels up from the location of the script for 'description.txt'.
    If it does not exist, creates 'description.txt' with the file name of the MP3
    file minus the .mp3 extension on the first line, followed by ' by \n'.
    """
    three_up_dir = script_dir.parent.parent.parent
    desc_path = three_up_dir / "description.txt"

    if not desc_path.exists():
        content = f"{mp3_file.stem} by \n"
        try:
            desc_path.parent.mkdir(parents=True, exist_ok=True)
            with open(desc_path, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"  [CREATED DESCRIPTION] Created '{desc_path.name}' at 3 levels up ({desc_path})")
            return True
        except Exception as e:
            print(f"  [DESCRIPTION ERROR] Failed to create '{desc_path}': {e}")
            return False
    return False


def convert_with_ffmpeg(mp3_path, wav_path, sample_rate=TARGET_SAMPLE_RATE):
    """
    Converts MP3 to WAV at sample_rate using ffmpeg command line tool.
    """
    ffmpeg_bin = shutil.which("ffmpeg") or "ffmpeg"
    cmd = [
        ffmpeg_bin,
        "-y",               # Overwrite output file without asking
        "-i", str(mp3_path),
        "-ar", str(sample_rate),
        str(wav_path)
    ]
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if result.returncode == 0:
            return True, "Converted via ffmpeg"
        else:
            return False, f"ffmpeg error: {result.stderr.strip()}"
    except Exception as e:
        return False, f"ffmpeg execution failed: {e}"


def convert_with_pydub(mp3_path, wav_path, sample_rate=TARGET_SAMPLE_RATE):
    """
    Converts MP3 to WAV at sample_rate using pydub.
    """
    try:
        from pydub import AudioSegment
        sound = AudioSegment.from_file(str(mp3_path), format="mp3")
        sound = sound.set_frame_rate(sample_rate)
        sound.export(str(wav_path), format="wav")
        return True, "Converted via pydub"
    except Exception as e:
        return False, f"pydub error: {e}"


def convert_with_librosa(mp3_path, wav_path, sample_rate=TARGET_SAMPLE_RATE):
    """
    Converts MP3 to WAV at sample_rate using librosa and soundfile.
    """
    try:
        import librosa
        import soundfile as sf
        y, sr = librosa.load(str(mp3_path), sr=sample_rate)
        sf.write(str(wav_path), y, sample_rate)
        return True, "Converted via librosa/soundfile"
    except Exception as e:
        return False, f"librosa error: {e}"


def convert_mp3_file(mp3_path, wav_path):
    """
    Attempts to convert an MP3 file to 44.1kHz WAV using available tools.
    """
    # 1. Try ffmpeg
    if shutil.which("ffmpeg"):
        success, msg = convert_with_ffmpeg(mp3_path, wav_path)
        if success:
            return True, msg

    # 2. Try pydub
    try:
        import pydub  # noqa: F401
        success, msg = convert_with_pydub(mp3_path, wav_path)
        if success:
            return True, msg
    except ImportError:
        pass

    # 3. Try librosa / soundfile
    try:
        import librosa  # noqa: F401
        import soundfile  # noqa: F401
        success, msg = convert_with_librosa(mp3_path, wav_path)
        if success:
            return True, msg
    except ImportError:
        pass

    # 4. Fallback: try ffmpeg command anyway in case it's in PATH environment without shutil detection
    success, msg = convert_with_ffmpeg(mp3_path, wav_path)
    if success:
        return True, msg

    return False, "No working conversion engine found (ffmpeg, pydub, or librosa/soundfile required)."


def main():
    script_dir = Path(__file__).resolve().parent
    print(f"Directory: {script_dir}")
    print(f"Target format: WAV @ {TARGET_SAMPLE_RATE / 1000.0:.1f} kHz ({TARGET_SAMPLE_RATE:,} Hz)\n")

    # Find all MP3 files in script_dir (case-insensitive)
    mp3_files = [
        f for f in script_dir.iterdir()
        if f.is_file() and f.suffix.lower() == ".mp3"
    ]

    if not mp3_files:
        print("No MP3 files found in the directory.")
    else:
        print(f"Found {len(mp3_files)} MP3 file(s). Converting...\n")

        converted_count = 0
        failed_count = 0

        for mp3_file in sorted(mp3_files, key=lambda p: p.name.lower()):
            ensure_description_txt(mp3_file, script_dir)
            wav_file = mp3_file.with_suffix(".wav")
            print(f"Processing: '{mp3_file.name}' -> '{wav_file.name}'...")

            success, msg = convert_mp3_file(mp3_file, wav_file)
            if success:
                print(f"  [SUCCESS] {msg}")
                converted_count += 1
            else:
                print(f"  [FAILED] {msg}")
                failed_count += 1

        print(f"\nConversion Summary:")
        print(f"  - Total MP3 files: {len(mp3_files)}")
        print(f"  - Successfully converted: {converted_count}")
        print(f"  - Failed: {failed_count}")

    print("\nExecution complete.")
    if sys.stdin and sys.stdin.isatty():
        try:
            input("Press Enter to exit...")
        except (KeyboardInterrupt, EOFError):
            pass


if __name__ == '__main__':
    main()

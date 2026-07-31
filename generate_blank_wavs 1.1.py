#!/usr/bin/env python3
"""Utility script for generating timestamped blank WAV files with FFmpeg."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import traceback
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, List, Optional

AUDIO_EXTENSIONS = {
    ".wav",
    ".mp3",
    ".flac",
    ".aif",
    ".aiff",
    ".ogg",
    ".oga",
    ".m4a",
    ".aac",
    ".wma",
    ".opus",
    ".pcm",
    ".alac",
}

NEW_TRACKS = [
    ("01", "sustained bass"),
    ("02", "sustained treble"),
    ("03", "transient bass"),
    ("04", "transient treble"),
]


@dataclass
class RenameTarget:
    original_path: Path
    number: int
    suffix: str


def ensure_tool_available(tool: str) -> None:
    if shutil.which(tool) is None:
        sys.exit(
            f"Required tool '{tool}' was not found in your PATH. "
            "Please install FFmpeg (which also provides ffprobe) and try again."
        )


def list_audio_files(directory: Path) -> List[Path]:
    return [
        path
        for path in directory.iterdir()
        if path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS
    ]


def collect_numbered_audio(files: Iterable[Path]) -> List[RenameTarget]:
    numbered: List[RenameTarget] = []
    for file_path in files:
        match = re.match(r"^(\d+)(.*)$", file_path.stem)
        if not match:
            continue
        number = int(match.group(1))
        suffix = match.group(2)
        numbered.append(RenameTarget(file_path, number, suffix))
    return numbered


def rename_numbered_audio_files(directory: Path) -> None:
    audio_files = list_audio_files(directory)
    numbered = collect_numbered_audio(audio_files)
    # Rename in descending order so that adding 4 never collides with existing prefixes.
    for target in sorted(numbered, key=lambda item: item.number, reverse=True):
        new_number = target.number + 4
        new_stem = f"{new_number:02d}{target.suffix}"
        destination = target.original_path.with_name(f"{new_stem}{target.original_path.suffix}")
        collision_counter = 1
        while destination.exists():
            destination = target.original_path.with_name(
                f"{new_stem} ({collision_counter}){target.original_path.suffix}"
            )
            collision_counter += 1
        if destination == target.original_path:
            continue
        target.original_path.rename(destination)


def probe_duration_seconds(path: Path) -> float:
    try:
        result = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-show_entries",
                "format=duration",
                "-of",
                "default=noprint_wrappers=1:nokey=1",
                str(path),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
    except subprocess.CalledProcessError as exc:  # pragma: no cover - defensive
        raise RuntimeError(f"ffprobe failed for {path.name}") from exc
    try:
        return float(result.stdout.strip())
    except ValueError as exc:  # pragma: no cover - defensive
        raise RuntimeError(f"Could not parse duration for {path.name}") from exc


def longest_audio_duration(directory: Path) -> float:
    audio_files = list_audio_files(directory)
    if not audio_files:
        print("No audio files found; using 120.0s default duration.")
        return 120.0
    durations = [probe_duration_seconds(path) for path in audio_files]
    longest = max(durations)
    if longest <= 0:
        print("Audio files found but none had a positive duration; using 120.0s default duration.")
        return 120.0
    return longest


def format_timestamp_token(moment: datetime) -> str:
    parts = [
        str(moment.year),
        str(moment.month),
        str(moment.day),
        str(moment.hour),
        str(moment.minute),
        str(moment.second),
    ]
    return "-".join(parts)


def create_blank_wav(output_path: Path, duration_seconds: float) -> None:
    if duration_seconds <= 0:
        sys.exit("Cannot create blank audio with non-positive duration.")
    if output_path.exists():
        output_path.unlink()
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            f"{duration_seconds:.6f}",
            "-acodec",
            "pcm_s16le",
            str(output_path),
        ],
        check=True,
    )


def extract_timestamp_from_filename(filename: str) -> Optional[str]:
    """Extract timestamp from filename if it exists in square brackets.
    Handles both formats: [2024-12-17-19-31-58] and [2024-12-17 193158]"""
    # Try hyphenated format first: [2024-12-17-19-31-58]
    match = re.search(r'\[(\d{4}-\d{1,2}-\d{1,2}-\d{1,2}-\d{1,2}-\d{1,2})\]', filename)
    if match:
        return match.group(1)
    
    # Try space format: [2024-12-17 193158]
    match = re.search(r'\[(\d{4}-\d{1,2}-\d{1,2} \d{6})\]', filename)
    return match.group(1) if match else None


def copy_audio_file(source_path: Path, destination_path: Path) -> None:
    """Copy audio file using FFmpeg to preserve quality."""
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-i",
            str(source_path),
            "-c",
            "copy",
            str(destination_path),
        ],
        check=True,
    )


def clear_audio_to_silence(audio_path: Path) -> None:
    """Replace audio content with silence while preserving duration."""
    duration = probe_duration_seconds(audio_path)
    subprocess.run(
        [
            "ffmpeg",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "anullsrc=r=44100:cl=mono",
            "-t",
            f"{duration:.6f}",
            "-acodec",
            "pcm_s16le",
            str(audio_path),
        ],
        check=True,
    )


def get_next_numbered_filename(directory: Path, base_name: str, suffix: str) -> str:
    """Get the next available numbered filename, incrementing existing numbers by 4."""
    # Extract existing number if present
    match = re.match(r'^(\d+)(.*)$', base_name)
    if match:
        current_number = int(match.group(1))
        name_suffix = match.group(2)
        next_number = current_number + 4
        new_base = f"{next_number:02d}{name_suffix}"
    else:
        # If no number, don't add one - just use the original name
        new_base = base_name
    
    # Check for collisions and add counter if needed
    counter = 1
    final_name = new_base
    while (directory / f"{final_name}{suffix}").exists():
        final_name = f"{new_base} ({counter})"
        counter += 1
    
    return f"{final_name}{suffix}"


def update_timestamp_in_filename(filename: str, new_timestamp: str) -> str:
    """Update or add timestamp in filename.
    Handles both formats: [2024-12-17-19-31-58] and [2024-12-17 193158]"""
    if '[' in filename and ']' in filename:
        # Try to replace hyphenated format first
        new_filename = re.sub(r'\[\d{4}-\d{1,2}-\d{1,2}-\d{1,2}-\d{1,2}-\d{1,2}\]', f'[{new_timestamp}]', filename)
        if new_filename != filename:
            return new_filename
        
        # Try to replace space format
        new_filename = re.sub(r'\[\d{4}-\d{1,2}-\d{1,2} \d{6}\]', f'[{new_timestamp}]', filename)
        if new_filename != filename:
            return new_filename
        
        # If no pattern matched, return original
        return filename
    else:
        # Add timestamp before extension
        stem = Path(filename).stem
        suffix = Path(filename).suffix
        return f"{stem} [{new_timestamp}]{suffix}"


def process_audio_files(file_paths: List[Path]) -> None:
    """Process audio files: copy with incremented numbers, clear originals, update timestamps."""
    new_timestamp_token = format_timestamp_token(datetime.now())
    
    for file_path in file_paths:
        if not file_path.exists():
            print(f"Warning: File {file_path} does not exist, skipping.")
            continue
        
        if file_path.suffix.lower() not in AUDIO_EXTENSIONS:
            print(f"Warning: {file_path.name} is not a supported audio format, skipping.")
            continue
        
        # First, copy the original file to the new name with new timestamp
        original_stem = file_path.stem
        # Check if original already has a timestamp
        existing_timestamp = extract_timestamp_from_filename(file_path.name)
        
        if existing_timestamp:
            # Original has timestamp, use the stem without timestamp for copy
            # Remove the existing timestamp from the stem
            stem_without_timestamp = re.sub(r'\s*\[\d{4}-\d{1,2}-\d{1,2}(-\d{1,2}){3}\]\s*$', '', original_stem)
            stem_without_timestamp = re.sub(r'\s*\[\d{4}-\d{1,2}-\d{1,2}\s\d{6}\]\s*$', '', stem_without_timestamp)
            copy_stem_with_timestamp = f"{stem_without_timestamp} [{new_timestamp_token}]"
        else:
            # Original has no timestamp, add timestamp to copy
            copy_stem_with_timestamp = f"{original_stem} [{new_timestamp_token}]"
        
        new_filename = get_next_numbered_filename(file_path.parent, copy_stem_with_timestamp, file_path.suffix)
        copy_path = file_path.parent / new_filename
        
        # Copy the file
        try:
            copy_audio_file(file_path, copy_path)
            print(f"Copied {file_path.name} to {copy_path.name}")
        except subprocess.CalledProcessError as e:
            print(f"Error copying {file_path.name}: {e}")
            continue
        
        # Clear the original file to silence (keeping original name unchanged)
        try:
            clear_audio_to_silence(file_path)
            print(f"Cleared {file_path.name} to silence")
        except subprocess.CalledProcessError as e:
            print(f"Error clearing {file_path.name}: {e}")


def main() -> None:
    ensure_tool_available("ffmpeg")
    ensure_tool_available("ffprobe")

    parser = argparse.ArgumentParser(description="Generate blank WAV files or process existing audio files.")
    parser.add_argument(
        "files",
        nargs="*",
        help="Audio files to process (copy, clear to silence, and update timestamps)"
    )
    args = parser.parse_args()

    if args.files:
        # Process provided audio files
        file_paths = [Path(f).resolve() for f in args.files]
        process_audio_files(file_paths)
    else:
        # Original behavior: create new blank WAV files
        script_dir = Path(__file__).resolve().parent

        rename_numbered_audio_files(script_dir)

        duration = longest_audio_duration(script_dir)
        timestamp_token = format_timestamp_token(datetime.now())

        for prefix, label in NEW_TRACKS:
            file_name = f"{prefix} {label} [{timestamp_token}].wav"
            output_path = script_dir / file_name
            create_blank_wav(output_path, duration)
            print(f"Created {output_path.name} ({duration:.2f}s)")


if __name__ == "__main__":  # pragma: no cover - entrypoint handling
    try:
        main()
        #input("Press Enter to exit...")
    except SystemExit as e:
        # Show exit reason/code and wait so a console window doesn't close immediately.
        print(f"SystemExit: {e}")
        traceback.print_exc()
        input("Press Enter to exit...")
        sys.exit(e.code if hasattr(e, "code") else 0)
    except Exception:
        # Print full traceback and wait for user to acknowledge before exiting.
        traceback.print_exc()
        input("Press Enter to exit...")
        sys.exit(1)

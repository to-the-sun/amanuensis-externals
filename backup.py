import os
import shutil
import time
import re
import json
import send2trash

# Default paths
PROJECTS_FOLDER = "D:/[Library]/[Audio]/[Works]/[Projects]"
DESTINATION_DRIVE = "F:/"
WATCHED_FOLDERS = {"stems", "Consolidate"}  # Folders to monitor
CHECK_INTERVAL = 3600  # Check every hour 

MISCELLANEOUS_FILES = {"specs.json", "top_sounds.txt", "description.txt", "master.als"}  # Files to sync regardless of folder

sync_queue = set()  # Queue to hold files to be synced

def is_audio_file(path):
    audio_extensions = {".wav", ".mp3", ".flac", ".aiff", ".ogg", ".m4a"}
    return os.path.splitext(path)[1].lower() in audio_extensions

def sync_file(src_path):
    try:
        relative_path = os.path.relpath(src_path, PROJECTS_FOLDER)
        dest_path = os.path.join(DESTINATION_DRIVE, relative_path)
        dest_dir = os.path.dirname(dest_path)
        
        os.makedirs(dest_dir, exist_ok=True)  # Create directories if needed
        shutil.copy2(src_path, dest_path)  # Copy file with metadata
        print(f"Synced {dest_path}")
        return True
    except Exception as e:
        print(f"Failed to sync {src_path}: {e}")
        return False

def scan_and_sync():
    current_time = time.time()
    for root, _, files in os.walk(PROJECTS_FOLDER):
        if any(folder in root.split(os.sep) for folder in WATCHED_FOLDERS):
            for file in files:
                file_path = os.path.join(root, file)
                if is_audio_file(file_path):
                    try:
                        modified_time = os.path.getmtime(file_path)
                        created_time = os.path.getctime(file_path)
                    except FileNotFoundError:
                        continue  # File might have been deleted before we could read it
                    
                    if current_time - modified_time <= CHECK_INTERVAL or current_time - created_time <= CHECK_INTERVAL:
                        relative_path = os.path.relpath(file_path, PROJECTS_FOLDER)
                        path_parts = relative_path.split(os.sep)
                        
                        # Ensure the audio file is within the monitored folders
                        if len(path_parts) > 1 and path_parts[-2] in WATCHED_FOLDERS:
                            sync_queue.add(file_path)
        
        # Check for miscellaneous files
        for file in files:
            if file in MISCELLANEOUS_FILES:
                file_path = os.path.join(root, file)
                try:
                    modified_time = os.path.getmtime(file_path)
                    created_time = os.path.getctime(file_path)
                except FileNotFoundError:
                    continue  # File might have been deleted before we could read it
                
                if current_time - modified_time <= CHECK_INTERVAL or current_time - created_time <= CHECK_INTERVAL:
                    sync_queue.add(file_path)

    # Process the sync queue
    for file_path in list(sync_queue):
        if sync_file(file_path):
            sync_queue.remove(file_path)

def is_palette_in_transcript(data, target_value):
    if isinstance(data, dict):
        for k, v in data.items():
            if k == "palette":
                if v == target_value:
                    return True
                if isinstance(v, (list, tuple, set)) and target_value in v:
                    return True
            if is_palette_in_transcript(v, target_value):
                return True
    elif isinstance(data, (list, tuple)):
        for item in data:
            if is_palette_in_transcript(item, target_value):
                return True
    return False

def local_cleanup():
    # Regex patterns for matching filenames
    patterns = [
        re.compile(r"^log \[\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}\]\.txt$"),
        re.compile(r"^palette_\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}\.wav$"),
        re.compile(r"^passes_\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}\.txt$"),
        re.compile(r"^scores_\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}-\d{1,4}\.txt$")
    ]
    for root, dirs, files in os.walk(PROJECTS_FOLDER):
        if os.path.basename(root) == "Backup":
            parent_folder = os.path.dirname(root)
            transcript_path = os.path.join(parent_folder, "transcript.json")
            transcript_data = None
            if os.path.exists(transcript_path):
                try:
                    with open(transcript_path, "r", encoding="utf-8") as f:
                        transcript_data = json.load(f)
                except Exception as e:
                    print(f"Failed to read {transcript_path}: {e}")

            for file in files:
                if any(p.match(file) for p in patterns):
                    parent_file_path = os.path.join(parent_folder, file)
                    parent_file_path = os.path.normpath(parent_file_path)  # Normalize path separators
                    if os.path.exists(parent_file_path):
                        if file.startswith("palette_"):
                            stripped_name = file[len("palette_"):]
                            if transcript_data is not None and is_palette_in_transcript(transcript_data, stripped_name):
                                print(f"Skipping recycling for {parent_file_path} (found in transcript.json)")
                                continue
                        try:
                            send2trash.send2trash(parent_file_path)
                            print(f"Moved to recycle bin: {parent_file_path}")
                        except Exception as e:
                            print(f"Failed to move {parent_file_path} to recycle bin: {e}")

if __name__ == "__main__":
    print(f"Checking '{PROJECTS_FOLDER}' for audio file changes every {CHECK_INTERVAL} seconds...")
    try:
        while True:
            scan_and_sync()
            local_cleanup()
            time.sleep(CHECK_INTERVAL)
    except KeyboardInterrupt:
        print("Stopped monitoring.")

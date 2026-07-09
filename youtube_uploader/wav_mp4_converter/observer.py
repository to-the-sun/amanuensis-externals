import time
import subprocess
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler

# Specify the folder to watch and the script to run
folder_to_watch = "D:/[Library]/[Audio]/[Works]"  # Replace with your desired folder path
script_to_run = "D:/[Library]/[Tools]/[Python]/youtube_uploader/wav_mp4_converter/wav_mp4_converter.py"  # Replace with the path to your script

class Watcher:
    def __init__(self):
        self.observer = Observer()

    def run(self):
        event_handler = Handler()
        self.observer.schedule(event_handler, folder_to_watch, recursive=False)
        self.observer.start()
        try:
            while True:
                time.sleep(5)
        except:
            self.observer.stop()
            print("Error occurred. Stopping observer.")

        self.observer.join()

class Handler(FileSystemEventHandler):
    def on_any_event(self, event):
        if event.is_directory:
            return None  # Ignore subdirectories

        elif event.event_type in ["created", "modified"]:
            if event.src_path.endswith(".wav"):
                print(f"File {event.src_path} has been {event.event_type}. Running script...")
                subprocess.run(["python", script_to_run])

if __name__ == "__main__":
    w = Watcher()
    w.run()

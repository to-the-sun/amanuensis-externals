import os
import sys
import socket
import subprocess
import threading
import time
from datetime import datetime

CONTROL_PORT = 9099

def get_timestamp_str():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

def log_msg(msg):
    print(f"[{get_timestamp_str()}] [RECORDER] {msg}", flush=True)

class VideoRecorderServer:
    def __init__(self):
        self.save_path = "."
        self.is_recording = False
        self.ffmpeg_proc = None

        # Connection that owns the active recording session
        self.recording_conn = None

        self.samplerate = 44100

        # Paths for current session
        self.temp_audio_path = ""
        self.temp_video_path = ""
        self.final_video_path = ""

        # Lock for synchronizing state
        self.lock = threading.Lock()

        # Auto-detect available GPU/HW acceleration options
        self.capture_format = "gdigrab"  # Fallback
        self.video_encoder = "libx264"    # Fallback
        self.detect_gpu_capabilities()

    def detect_gpu_capabilities(self):
        """
        Queries ffmpeg to detect supported hardware accelerators, encoders, and capture inputs.
        """
        log_msg("=== DETECTING SYSTEM GPU CAPABILITIES ===")
        try:
            # Check for encoders
            result = subprocess.run(
                ["ffmpeg", "-encoders"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
                timeout=5.0
            )
            encoders_out = result.stdout

            # Prioritize Nvidia NVENC, then AMD AMF, then Intel QSV, then Windows Media Foundation (MF)
            if "h264_nvenc" in encoders_out:
                self.video_encoder = "h264_nvenc"
                log_msg("SUCCESS: Detected Nvidia NVENC GPU encoder.")
            elif "h264_amf" in encoders_out:
                self.video_encoder = "h264_amf"
                log_msg("SUCCESS: Detected AMD AMF GPU encoder.")
            elif "h264_qsv" in encoders_out:
                self.video_encoder = "h264_qsv"
                log_msg("SUCCESS: Detected Intel QSV hardware encoder.")
            elif "h264_mf" in encoders_out:
                self.video_encoder = "h264_mf"
                log_msg("SUCCESS: Detected Media Foundation hardware encoder.")
            else:
                self.video_encoder = "libx264"
                log_msg("INFO: No GPU video encoder detected. Falling back to libx264 CPU encoder.")

            # Check if DXGI Desktop Duplication (ddagrab) or x11grab is supported for capturing
            result_formats = subprocess.run(
                ["ffmpeg", "-formats"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0,
                timeout=5.0
            )
            formats_out = result_formats.stdout
            if "ddagrab" in formats_out:
                self.capture_format = "ddagrab"
                log_msg("SUCCESS: Detected GPU-accelerated DXGI (ddagrab) screen capture format.")
            elif "x11grab" in formats_out:
                self.capture_format = "x11grab"
                log_msg("SUCCESS: Detected Linux X11 (x11grab) screen capture format.")
            else:
                self.capture_format = "gdigrab"
                log_msg("INFO: Using standard Windows GDI (gdigrab) screen capture format.")

        except Exception as e:
            log_msg(f"WARNING: Error detecting GPU capabilities: {e}. Falling back to default CPU/GDI formats.")
            self.capture_format = "gdigrab"
            self.video_encoder = "libx264"
        log_msg("=========================================")

    def start_control_server(self):
        control_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        control_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            control_socket.bind(('127.0.0.1', CONTROL_PORT))
        except Exception as e:
            log_msg(f"FATAL ERROR: Failed to bind control socket to port {CONTROL_PORT}: {e}")
            sys.exit(1)

        control_socket.listen(5)
        log_msg(f"STATUS: Control TCP server is now listening on 127.0.0.1:{CONTROL_PORT}")

        while True:
            try:
                conn, addr = control_socket.accept()
                log_msg(f"CONNECTION: New client connected to control port from {addr}")
                threading.Thread(target=self.handle_control_client, args=(conn,), daemon=True).start()
            except Exception as e:
                log_msg(f"ERROR: Exception accepting control connection: {e}")
                break

    def handle_control_client(self, conn):
        buffer = ""
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    log_msg("CONNECTION: Control client disconnected gracefully.")
                    # Only stop recording if the disconnected client was the one who started it!
                    with self.lock:
                        if self.is_recording and self.recording_conn == conn:
                            log_msg("WARNING: Recording owner client disconnected. Stopping recording automatically.")
                            self.stop_recording_internal()
                    break

                buffer += data.decode('utf-8', errors='ignore')
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue

                    self.process_command(line, conn)
        except Exception as e:
            log_msg(f"ERROR: Exception in control client receiver: {e}")
            # Only stop recording if the client that threw the exception was the recording owner!
            with self.lock:
                if self.is_recording and self.recording_conn == conn:
                    log_msg("WARNING: Recording owner client connection threw exception. Stopping recording automatically.")
                    self.stop_recording_internal()
        finally:
            conn.close()

    def process_command(self, cmd_line, conn):
        log_msg(f"COMMAND RECEIVED: '{cmd_line}'")

        # Split command and argument string robustly with maxsplit=1
        parts = cmd_line.split(" ", 1)
        cmd = parts[0].upper()
        args = parts[1].strip() if len(parts) > 1 else ""

        if cmd == "PATH":
            if not args:
                log_msg("ERROR: PATH command missing target directory path")
                conn.sendall(b"ERROR: Missing path argument\n")
                return
            new_path = args
            # Clean quotes if present
            if (new_path.startswith('"') and new_path.endswith('"')) or (new_path.startswith("'") and new_path.endswith("'")):
                new_path = new_path[1:-1]

            with self.lock:
                self.save_path = new_path
                try:
                    os.makedirs(self.save_path, exist_ok=True)
                    log_msg(f"ACTION: Save path successfully set and verified to: '{self.save_path}'")
                    conn.sendall(b"PATH_SET\n")
                except Exception as e:
                    log_msg(f"ERROR: Failed to create save directory '{self.save_path}': {e}")
                    conn.sendall(f"ERROR: Failed to create directory: {e}\n".encode('utf-8'))

        elif cmd == "START":
            if not args:
                log_msg("ERROR: START command missing arguments")
                conn.sendall(b"ERROR: Missing timestamp or samplerate arguments\n")
                return

            sub_parts = args.split(" ", 1)
            if len(sub_parts) < 2:
                log_msg("ERROR: START command missing samplerate argument")
                conn.sendall(b"ERROR: Missing samplerate argument\n")
                return

            timestamp = sub_parts[0].strip()
            try:
                sr = int(sub_parts[1].strip())
            except ValueError:
                sr = 44100

            log_msg(f"ACTION: Starting record handshake for session '{timestamp}' @ {sr}Hz")
            success, msg = self.start_recording(timestamp, sr, conn)
            if success:
                log_msg(f"SUCCESS: Recording session '{timestamp}' successfully started.")
                conn.sendall(b"STARTED\n")
            else:
                log_msg(f"ERROR: Recording session '{timestamp}' failed to start: {msg}")
                conn.sendall(f"ERROR: {msg}\n".encode('utf-8'))

        elif cmd == "STOP":
            log_msg("ACTION: Stopping active recording session...")
            success, result_path_or_err = self.stop_recording()
            if success:
                log_msg(f"SUCCESS: Recording saved and muxed successfully to: '{result_path_or_err}'")
                conn.sendall(f"STOPPED {result_path_or_err}\n".encode('utf-8'))
            else:
                log_msg(f"ERROR: Recording failed to stop/mux: {result_path_or_err}")
                conn.sendall(f"ERROR {result_path_or_err}\n".encode('utf-8'))
        else:
            log_msg(f"WARNING: Unknown command received: '{cmd}'")
            conn.sendall(f"ERROR: Unknown command: {cmd}\n".encode('utf-8'))

    def start_recording(self, timestamp, samplerate, conn):
        with self.lock:
            if self.is_recording:
                return False, "Already recording"

            self.samplerate = samplerate

            # Store temporary intermediate video & audio files directly in the target destination folder,
            # as explicitly requested by the user.
            self.temp_audio_path = os.path.join(self.save_path, f"temp_audio_{timestamp}.wav").replace("\\", "/")
            self.temp_video_path = os.path.join(self.save_path, f"temp_video_{timestamp}.mp4").replace("\\", "/")
            self.final_video_path = os.path.join(self.save_path, f"video_{timestamp}.mp4").replace("\\", "/")

            log_msg(f"CONFIG: Temp Audio WAV Path: '{self.temp_audio_path}'")
            log_msg(f"CONFIG: Temp Video MP4 Path: '{self.temp_video_path}'")
            log_msg(f"CONFIG: Final Video MP4 Path: '{self.final_video_path}'")

            # Start ffmpeg screen recording with optimized GPU capabilities
            cmd = [
                'ffmpeg', '-y'
            ]

            if self.capture_format == "ddagrab":
                cmd += [
                    '-f', 'ddagrab',
                    '-framerate', '30',
                    '-i', 'desktop'
                ]
            elif self.capture_format == "x11grab":
                cmd += [
                    '-f', 'x11grab',
                    '-framerate', '30',
                    '-i', ':0.0'
                ]
            else:
                cmd += [
                    '-f', 'gdigrab',
                    '-framerate', '30',
                    '-i', 'desktop'
                ]

            # Encoding settings with GPU acceleration
            cmd += [
                '-c:v', self.video_encoder
            ]

            if self.video_encoder == "h264_nvenc":
                cmd += [
                    '-preset', 'p1',       # Fastest/lowest latency preset
                    '-tune', 'ull',        # Ultra low latency
                    '-pix_fmt', 'yuv420p'
                ]
            elif self.video_encoder == "h264_amf":
                cmd += [
                    '-quality', 'speed',
                    '-pix_fmt', 'yuv420p'
                ]
            elif self.video_encoder == "h264_qsv":
                cmd += [
                    '-preset', 'veryfast',
                    '-pix_fmt', 'nv12'
                ]
            elif self.video_encoder == "h264_mf":
                cmd += [
                    '-pix_fmt', 'yuv420p'
                ]
            else:
                cmd += [
                    '-pix_fmt', 'yuv420p'
                ]

            cmd.append(self.temp_video_path)

            log_msg(f"SUBPROCESS: Spawning FFmpeg capture: {' '.join(cmd)}")

            try:
                self.ffmpeg_proc = subprocess.Popen(
                    cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
                )
                self.is_recording = True
                self.recording_conn = conn  # Set this connection as the owner of the active recording session
                log_msg(f"SUBPROCESS: FFmpeg capture spawned with PID {self.ffmpeg_proc.pid}")
            except Exception as e:
                log_msg(f"SUBPROCESS ERROR: Failed to start ffmpeg: {e}")
                self.is_recording = False
                self.recording_conn = None
                return False, f"Failed to start ffmpeg: {e}"

            return True, "Recording started"

    def stop_recording(self):
        with self.lock:
            if not self.is_recording:
                log_msg("WARNING: Stop requested but no recording session is active.")
                return False, "Not recording"
            return self.stop_recording_internal()

    def stop_recording_internal(self):
        log_msg("ACTION: Stopping recording internally and releasing all file/socket resources...")
        self.is_recording = False
        self.recording_conn = None  # Clear owner connection

        # 1. Stop ffmpeg screen recording gracefully
        if self.ffmpeg_proc:
            log_msg(f"SUBPROCESS: Gracefully terminating FFmpeg capture PID {self.ffmpeg_proc.pid}...")
            try:
                # Send 'q' to gracefully stop recording
                self.ffmpeg_proc.stdin.write(b"q\n")
                self.ffmpeg_proc.stdin.flush()
                log_msg("SUBPROCESS: Sent 'q' signal to FFmpeg stdin.")
                # Wait for it to finish
                self.ffmpeg_proc.wait(timeout=10.0)
                log_msg("SUBPROCESS: FFmpeg process exited successfully.")
            except Exception as e:
                log_msg(f"SUBPROCESS WARNING: Error stopping ffmpeg gracefully: {e}. Terminating forcibly.")
                try:
                    self.ffmpeg_proc.terminate()
                except:
                    pass
            self.ffmpeg_proc = None

        # Give Max a split second to finish flushing and closing its WAV file
        time.sleep(0.5)

        # 2. Audio-Video Muxing
        mux_cmd = [
            'ffmpeg', '-y',
            '-i', self.temp_video_path,
            '-i', self.temp_audio_path,
            '-c:v', 'copy',
            '-c:a', 'aac',
            '-shortest',
            self.final_video_path
        ]

        mux_success = False
        try:
            log_msg(f"SUBPROCESS: Muxing audio & video: {' '.join(mux_cmd)}")
            mux_proc = subprocess.Popen(
                mux_cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
            )
            mux_proc.wait(timeout=15.0)
            if mux_proc.returncode == 0:
                mux_success = True
                log_msg("SUBPROCESS: Muxing completed successfully with exit code 0.")
            else:
                log_msg(f"SUBPROCESS ERROR: Muxing failed with exit code {mux_proc.returncode}")
        except Exception as e:
            log_msg(f"SUBPROCESS ERROR: Exception during muxing: {e}")

        # 3. Clean up temp files
        log_msg("ACTION: Cleaning up temporary WAV and raw MP4 files from destination folder...")
        try:
            if os.path.exists(self.temp_audio_path):
                os.remove(self.temp_audio_path)
                log_msg(f"CLEANUP: Removed temporary WAV file: '{self.temp_audio_path}'")
            if os.path.exists(self.temp_video_path):
                os.remove(self.temp_video_path)
                log_msg(f"CLEANUP: Removed temporary raw MP4 file: '{self.temp_video_path}'")
        except Exception as e:
            log_msg(f"CLEANUP WARNING: Error cleaning up temp files: {e}")

        if mux_success and os.path.exists(self.final_video_path):
            log_msg(f"SUCCESS: Recording fully generated! Final file is at: '{self.final_video_path}'")
            return True, self.final_video_path
        else:
            log_msg("ERROR: Video recording session finished, but muxing failed or final file is missing.")
            return False, "Muxing failed or final file not found"

if __name__ == "__main__":
    # Ensure stdout/stderr are unbuffered for immediate log delivery
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)

    log_msg("=========================================")
    log_msg("STARTING RECORDER SERVER PROCESS")
    log_msg(f"Working Directory: {os.getcwd()}")
    log_msg(f"Python Executable: {sys.executable}")
    log_msg("=========================================")

    server = VideoRecorderServer()
    server.start_control_server()

import os
import sys
import socket
import subprocess
import threading
import wave
import time

CONTROL_PORT = 9099
AUDIO_PORT = 9100

class VideoRecorderServer:
    def __init__(self):
        self.save_path = "."
        self.is_recording = False
        self.ffmpeg_proc = None

        # Audio streaming states
        self.audio_thread = None
        self.audio_socket = None
        self.audio_conn = None
        self.wav_file = None
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
        print("Recorder: Detecting GPU and hardware acceleration capabilities...", flush=True)
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
                print("Recorder: Detected Nvidia NVENC GPU encoder.", flush=True)
            elif "h264_amf" in encoders_out:
                self.video_encoder = "h264_amf"
                print("Recorder: Detected AMD AMF GPU encoder.", flush=True)
            elif "h264_qsv" in encoders_out:
                self.video_encoder = "h264_qsv"
                print("Recorder: Detected Intel QSV hardware encoder.", flush=True)
            elif "h264_mf" in encoders_out:
                self.video_encoder = "h264_mf"
                print("Recorder: Detected Media Foundation hardware encoder.", flush=True)
            else:
                self.video_encoder = "libx264"
                print("Recorder: No GPU video encoder detected. Using libx264 CPU encoder.", flush=True)

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
                print("Recorder: Detected GPU-accelerated DXGI (ddagrab) screen capture format.", flush=True)
            elif "x11grab" in formats_out:
                self.capture_format = "x11grab"
                print("Recorder: Detected Linux X11 screen capture format.", flush=True)
            else:
                self.capture_format = "gdigrab"
                print("Recorder: Using standard GDI screen capture format.", flush=True)

        except Exception as e:
            print(f"Recorder: Error detecting GPU capabilities: {e}. Falling back to defaults.", flush=True)
            self.capture_format = "gdigrab"
            self.video_encoder = "libx264"

    def start_control_server(self):
        control_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        control_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            control_socket.bind(('127.0.0.1', CONTROL_PORT))
        except Exception as e:
            print(f"Recorder: Failed to bind control socket to port {CONTROL_PORT}: {e}", flush=True)
            sys.exit(1)

        control_socket.listen(5)
        print(f"Recorder: Control server listening on 127.0.0.1:{CONTROL_PORT}", flush=True)

        while True:
            try:
                conn, addr = control_socket.accept()
                threading.Thread(target=self.handle_control_client, args=(conn,), daemon=True).start()
            except Exception as e:
                print(f"Recorder: Error accepting control connection: {e}", flush=True)
                break

    def handle_control_client(self, conn):
        print("Recorder: Control client connected.", flush=True)
        buffer = ""
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    print("Recorder: Control client disconnected.", flush=True)
                    # If we were recording and client disconnected unexpectedly, stop gracefully
                    with self.lock:
                        if self.is_recording:
                            print("Recorder: Client disconnected while recording. Stopping recording gracefully.", flush=True)
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
            print(f"Recorder: Exception in control client handler: {e}", flush=True)
            with self.lock:
                if self.is_recording:
                    self.stop_recording_internal()
        finally:
            conn.close()

    def process_command(self, cmd_line, conn):
        print(f"Recorder: Received command: '{cmd_line}'", flush=True)
        parts = cmd_line.split(" ", 2)
        cmd = parts[0].upper()

        if cmd == "PATH":
            if len(parts) < 2:
                conn.sendall(b"ERROR: Missing path argument\n")
                return
            new_path = parts[1].strip()
            # Clean quotes if present
            if (new_path.startswith('"') and new_path.endswith('"')) or (new_path.startswith("'") and new_path.endswith("'")):
                new_path = new_path[1:-1]

            with self.lock:
                self.save_path = new_path
                try:
                    os.makedirs(self.save_path, exist_ok=True)
                    print(f"Recorder: Save path set and verified: {self.save_path}", flush=True)
                    conn.sendall(b"PATH_SET\n")
                except Exception as e:
                    print(f"Recorder: Failed to create directory {self.save_path}: {e}", flush=True)
                    conn.sendall(f"ERROR: Failed to create directory: {e}\n".encode('utf-8'))

        elif cmd == "START":
            if len(parts) < 3:
                conn.sendall(b"ERROR: Missing timestamp or samplerate arguments\n")
                return
            timestamp = parts[1].strip()
            try:
                sr = int(parts[2].strip())
            except ValueError:
                sr = 44100

            success, msg = self.start_recording(timestamp, sr)
            if success:
                conn.sendall(b"STARTED\n")
            else:
                conn.sendall(f"ERROR: {msg}\n".encode('utf-8'))

        elif cmd == "STOP":
            success, result_path_or_err = self.stop_recording()
            if success:
                conn.sendall(f"STOPPED {result_path_or_err}\n".encode('utf-8'))
            else:
                conn.sendall(f"ERROR {result_path_or_err}\n".encode('utf-8'))
        else:
            conn.sendall(f"ERROR: Unknown command: {cmd}\n".encode('utf-8'))

    def start_recording(self, timestamp, samplerate):
        with self.lock:
            if self.is_recording:
                return False, "Already recording"

            self.samplerate = samplerate
            self.temp_audio_path = os.path.join(self.save_path, f"temp_audio_{timestamp}.wav").replace("\\", "/")
            self.temp_video_path = os.path.join(self.save_path, f"temp_video_{timestamp}.mp4").replace("\\", "/")
            self.final_video_path = os.path.join(self.save_path, f"video_{timestamp}.mp4").replace("\\", "/")

            print(f"Recorder: Starting recording session.", flush=True)
            print(f"Recorder: Temp Audio Path: {self.temp_audio_path}", flush=True)
            print(f"Recorder: Temp Video Path: {self.temp_video_path}", flush=True)
            print(f"Recorder: Final Video Path: {self.final_video_path}", flush=True)

            # 1. Open the wave file writer
            try:
                self.wav_file = wave.open(self.temp_audio_path, 'wb')
                self.wav_file.setnchannels(1)  # Mono
                self.wav_file.setsampwidth(2)  # 16-bit PCM (2 bytes)
                self.wav_file.setframerate(self.samplerate)
            except Exception as e:
                return False, f"Failed to create wave file: {e}"

            # 2. Setup audio TCP listener socket
            self.audio_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.audio_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                self.audio_socket.bind(('127.0.0.1', AUDIO_PORT))
                self.audio_socket.listen(1)
            except Exception as e:
                self.wav_file.close()
                os.remove(self.temp_audio_path)
                return False, f"Failed to bind audio socket to port {AUDIO_PORT}: {e}"

            # 3. Start audio listener thread
            self.is_recording = True
            self.audio_thread = threading.Thread(target=self.audio_receiver_loop, daemon=True)
            self.audio_thread.start()

            # 4. Start ffmpeg screen recording with optimized GPU capabilities
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
            # Add parameters specific to the selected hardware encoder to maximize GPU usage
            cmd += [
                '-c:v', self.video_encoder
            ]

            if self.video_encoder == "h264_nvenc":
                # High performance Nvidia NVENC settings (low latency, high quality presets, GPU processing)
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

            print(f"Recorder: Running ffmpeg command: {' '.join(cmd)}", flush=True)

            try:
                self.ffmpeg_proc = subprocess.Popen(
                    cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
                )
            except Exception as e:
                self.is_recording = False
                self.audio_socket.close()
                self.wav_file.close()
                try:
                    os.remove(self.temp_audio_path)
                except:
                    pass
                return False, f"Failed to start ffmpeg: {e}"

            return True, "Recording started"

    def audio_receiver_loop(self):
        print("Recorder: Audio receiver loop active. Waiting for connection on port 9100...", flush=True)
        try:
            self.audio_socket.settimeout(5.0)  # Wait up to 5 seconds for Max to connect
            conn, addr = self.audio_socket.accept()
            self.audio_socket.settimeout(None)
            self.audio_conn = conn
            print("Recorder: Audio stream connected.", flush=True)

            while True:
                # Receive raw 16-bit PCM samples
                data = conn.recv(65536)
                if not data:
                    print("Recorder: Audio stream disconnected.", flush=True)
                    break

                with self.lock:
                    if self.wav_file:
                        self.wav_file.writeframes(data)
        except socket.timeout:
            print("Recorder: Timed out waiting for audio stream connection from Max.", flush=True)
        except Exception as e:
            print(f"Recorder: Error in audio receiver loop: {e}", flush=True)
        finally:
            if self.audio_conn:
                try:
                    self.audio_conn.close()
                except:
                    pass
            print("Recorder: Audio receiver loop finished.", flush=True)

    def stop_recording(self):
        with self.lock:
            if not self.is_recording:
                return False, "Not recording"
            return self.stop_recording_internal()

    def stop_recording_internal(self):
        print("Recorder: Stopping recording session...", flush=True)
        self.is_recording = False

        # 1. Stop audio socket & receiver
        if self.audio_conn:
            try:
                self.audio_conn.close()
            except:
                pass
            self.audio_conn = None

        if self.audio_socket:
            try:
                self.audio_socket.close()
            except:
                pass
            self.audio_socket = None

        if self.audio_thread:
            self.audio_thread.join(timeout=2.0)
            self.audio_thread = None

        if self.wav_file:
            try:
                self.wav_file.close()
            except:
                pass
            self.wav_file = None

        # 2. Stop ffmpeg screen recording gracefully
        if self.ffmpeg_proc:
            try:
                # Send 'q' to gracefully stop recording
                self.ffmpeg_proc.stdin.write(b"q\n")
                self.ffmpeg_proc.stdin.flush()
                # Wait for it to finish
                self.ffmpeg_proc.wait(timeout=10.0)
            except Exception as e:
                print(f"Recorder: Error stopping ffmpeg gracefully: {e}. Terminating.", flush=True)
                try:
                    self.ffmpeg_proc.terminate()
                except:
                    pass
            self.ffmpeg_proc = None

        # 3. Audio-Video Muxing
        # We can also hardware-accelerate audio encoding or let ffmpeg use copy on video and rapid aac on audio
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
            print("Recorder: Muxing video and audio streams...", flush=True)
            mux_proc = subprocess.Popen(
                mux_cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
            )
            mux_proc.wait(timeout=15.0)
            if mux_proc.returncode == 0:
                mux_success = True
        except Exception as e:
            print(f"Recorder: Error during muxing process: {e}", flush=True)

        # 4. Clean up temp files
        try:
            if os.path.exists(self.temp_audio_path):
                os.remove(self.temp_audio_path)
            if os.path.exists(self.temp_video_path):
                os.remove(self.temp_video_path)
        except Exception as e:
            print(f"Recorder: Error cleaning up temp files: {e}", flush=True)

        if mux_success and os.path.exists(self.final_video_path):
            print(f"Recorder: Recording saved successfully to: {self.final_video_path}", flush=True)
            return True, self.final_video_path
        else:
            return False, "Muxing failed or final file not found"

if __name__ == "__main__":
    # Ensure stdout/stderr are unbuffered for immediate log delivery
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)

    server = VideoRecorderServer()
    server.start_control_server()

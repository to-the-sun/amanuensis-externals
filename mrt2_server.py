import socket
import json
import struct
import threading
import time
import numpy as np
import sys

# Attempt to import Magenta RealTime 2
try:
    from magenta_rt.inference import MRT2
    MAGENTA_AVAILABLE = True
except ImportError:
    MAGENTA_AVAILABLE = False
    print("Magenta RealTime 2 library not found. Running in mock mode.")

class MRT2Bridge:
    def __init__(self, host='127.0.0.1', port=9998):
        self.host = host
        self.port = port
        self.running = False
        self.model_name = "mrt2_small"
        self.prompt = ""
        self.sr = 48000
        self.channels = 2

        self.mrt2 = None
        if MAGENTA_AVAILABLE:
            # We would initialize the model here if we had the checkpoints
            pass

        # Mock synth state
        self.phase = 0.0
        self.freq = 440.0
        self.amplitude = 0.0
        self.target_amplitude = 0.0

    def start(self):
        self.running = True
        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind((self.host, self.port))
        server_sock.listen(1)
        print(f"MRT2 Bridge listening on {self.host}:{self.port}")

        while self.running:
            try:
                server_sock.settimeout(1.0)
                conn, addr = server_sock.accept()
                print(f"Connected by {addr}")
                self.handle_client(conn)
            except socket.timeout:
                continue
            except Exception as e:
                print(f"Server error: {e}")
                break

    def handle_client(self, conn):
        stop_event = threading.Event()
        stream_thread = threading.Thread(target=self.stream_audio, args=(conn, stop_event))
        stream_thread.start()

        buffer = ""
        try:
            while self.running:
                data = conn.recv(1024).decode('utf-8')
                if not data:
                    break

                buffer += data
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    if not line:
                        continue
                    try:
                        event = json.loads(line)
                        self.process_event(event, conn)
                    except json.JSONDecodeError:
                        print(f"Invalid JSON: {line}")
        except Exception as e:
            print(f"Client error: {e}")
        finally:
            print("Client disconnected")
            stop_event.set()
            stream_thread.join()
            conn.close()

    def process_event(self, event, conn):
        ev_type = event.get("event")
        if ev_type == "prompt":
            self.prompt = event.get("text")
            print(f"New prompt: {self.prompt}")
            if self.mrt2:
                # self.mrt2.set_prompt(self.prompt)
                pass
            self.send_status(conn, f"Prompt set to: {self.prompt}")
        elif ev_type == "model":
            self.model_name = event.get("name")
            print(f"Model changed to: {self.model_name}")
            # Here we would reload the model if necessary
            self.send_status(conn, f"Model set to: {self.model_name}")
        elif ev_type == "midi":
            midi_data = event.get("data")
            if midi_data and len(midi_data) >= 3:
                status = midi_data[0]
                note = midi_data[1]
                vel = midi_data[2]

                if (status & 0xF0) == 0x90 and vel > 0: # Note On
                    self.freq = 440.0 * (2.0 ** ((note - 69) / 12.0))
                    self.target_amplitude = vel / 127.0
                    if self.mrt2:
                        # self.mrt2.note_on(note, vel)
                        pass
                elif (status & 0xF0) == 0x80 or ((status & 0xF0) == 0x90 and vel == 0): # Note Off
                    self.target_amplitude = 0.0
                    if self.mrt2:
                        # self.mrt2.note_off(note)
                        pass

    def send_status(self, conn, message):
        msg_bytes = message.encode('utf-8')
        header = struct.pack('<i', -len(msg_bytes))
        try:
            conn.sendall(header + msg_bytes)
        except:
            pass

    def stream_audio(self, conn, stop_event):
        chunk_size = 480 # 10ms at 48kHz
        while not stop_event.is_set():
            if self.mrt2:
                # audio_data = self.mrt2.generate(chunk_size)
                # This would return float32 interleaved audio
                pass

            # Fallback/Mock logic
            t = np.arange(chunk_size) / self.sr
            if self.amplitude < self.target_amplitude:
                self.amplitude = min(self.target_amplitude, self.amplitude + 0.05)
            elif self.amplitude > self.target_amplitude:
                self.amplitude = max(self.target_amplitude, self.amplitude - 0.01)

            samples = self.amplitude * np.sin(2 * np.pi * self.freq * (self.phase + t))
            self.phase += chunk_size / self.sr

            audio_data = np.zeros(chunk_size * 2, dtype=np.float32)
            audio_data[0::2] = samples
            audio_data[1::2] = samples

            bytes_data = audio_data.tobytes()
            header = struct.pack('<i', len(bytes_data))
            try:
                conn.sendall(header + bytes_data)
            except:
                break

            time.sleep(0.01)

if __name__ == "__main__":
    bridge = MRT2Bridge()
    bridge.start()

import socket
import json
import struct
import threading
import time
import numpy as np
import sys
import os
import pygame

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
        self.midi_notes = {} # note -> velocity

        # GUI State
        self.gui_enabled = False
        self.audio_level = 0.0

    def start(self):
        self.running = True
        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server_sock.bind((self.host, self.port))
            server_sock.listen(5)
            print(f"MRT2 Bridge listening on {self.host}:{self.port}")
        except Exception as e:
            print(f"FAILED to start server on {self.host}:{self.port} - {e}")
            return

        # Start GUI in background thread if needed, but pygame usually likes the main thread
        # We'll run the socket loop in a thread and pygame in main
        threading.Thread(target=self.accept_loop, args=(server_sock,), daemon=True).start()

        self.run_gui()

    def accept_loop(self, server_sock):
        while self.running:
            try:
                server_sock.settimeout(1.0)
                conn, addr = server_sock.accept()
                print(f"Connected by {addr}")
                self.handle_client(conn)
            except socket.timeout:
                continue
            except Exception as e:
                print(f"Accept loop error: {e}")
                break

    def handle_client(self, conn):
        stop_event = threading.Event()
        stream_thread = threading.Thread(target=self.stream_audio, args=(conn, stop_event))
        stream_thread.start()

        buffer = ""
        try:
            while self.running:
                data = conn.recv(4096).decode('utf-8')
                if not data:
                    print("Client closed connection")
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
            print(f"Client handling error: {e}")
        finally:
            stop_event.set()
            stream_thread.join()
            conn.close()
            print("Client cleanup complete")

    def process_event(self, event, conn):
        ev_type = event.get("event")
        print(f"Event: {ev_type}")
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
            self.send_status(conn, f"Model set to: {self.model_name}")
        elif ev_type == "open_gui":
            print("Request to open GUI received")
            self.gui_enabled = True
        elif ev_type == "midi":
            midi_data = event.get("data")
            if midi_data and len(midi_data) >= 3:
                status = midi_data[0]
                note = midi_data[1]
                vel = midi_data[2]

                if (status & 0xF0) == 0x90 and vel > 0: # Note On
                    self.freq = 440.0 * (2.0 ** ((note - 69) / 12.0))
                    self.target_amplitude = vel / 127.0
                    self.midi_notes[note] = vel
                    if self.mrt2:
                        # self.mrt2.note_on(note, vel)
                        pass
                elif (status & 0xF0) == 0x80 or ((status & 0xF0) == 0x90 and vel == 0): # Note Off
                    self.target_amplitude = 0.0
                    if note in self.midi_notes:
                        del self.midi_notes[note]
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
        while not stop_event.is_set() and self.running:
            # Generate Audio
            t = np.arange(chunk_size) / self.sr
            if self.amplitude < self.target_amplitude:
                self.amplitude = min(self.target_amplitude, self.amplitude + 0.1)
            elif self.amplitude > self.target_amplitude:
                self.amplitude = max(self.target_amplitude, self.amplitude - 0.05)

            samples = self.amplitude * np.sin(2 * np.pi * self.freq * (self.phase + t))
            self.phase += chunk_size / self.sr

            self.audio_level = self.amplitude # Update level for GUI

            audio_data = np.zeros(chunk_size * 2, dtype=np.float32)
            audio_data[0::2] = samples
            audio_data[1::2] = samples

            bytes_data = audio_data.tobytes()
            header = struct.pack('<i', len(bytes_data))
            try:
                conn.sendall(header + bytes_data)
            except:
                break

            time.sleep(0.008) # Slightly faster than 10ms to keep buffer ahead

    def run_gui(self):
        pygame.init()
        screen = None
        clock = pygame.time.Clock()
        font = pygame.font.SysFont("Arial", 16)

        print("GUI engine ready. Click 'open' in Max to show the window.")

        while self.running:
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    self.gui_enabled = False
                    if screen:
                        pygame.display.quit()
                        screen = None

            if self.gui_enabled:
                if not screen:
                    screen = pygame.display.set_mode((400, 300))
                    pygame.display.set_caption("MRT2 Bridge GUI")

                screen.fill((40, 40, 45))

                # Draw Title
                title = font.render(f"MRT2 Bridge - {self.model_name}", True, (200, 200, 200))
                screen.blit(title, (20, 20))

                # Draw Prompt
                p_text = font.render(f"Prompt: {self.prompt}", True, (150, 150, 255))
                screen.blit(p_text, (20, 50))

                # Draw Audio Meter
                pygame.draw.rect(screen, (60, 60, 60), (20, 100, 360, 20))
                meter_w = int(360 * self.audio_level)
                pygame.draw.rect(screen, (100, 255, 100), (20, 100, meter_w, 20))

                # Draw MIDI Notes
                y = 140
                notes_lbl = font.render("Active MIDI Notes:", True, (200, 200, 200))
                screen.blit(notes_lbl, (20, y))
                y += 25
                for note, vel in list(self.midi_notes.items()):
                    note_lbl = font.render(f"Note {note} (Vel: {vel})", True, (255, 255, 100))
                    screen.blit(note_lbl, (40, y))
                    y += 20

                if not MAGENTA_AVAILABLE:
                    warn_lbl = font.render("RUNNING IN MOCK MODE (No MRT2 Lib)", True, (255, 100, 100))
                    screen.blit(warn_lbl, (20, 260))

                pygame.display.flip()

            clock.tick(30)

if __name__ == "__main__":
    bridge = MRT2Bridge()
    bridge.start()

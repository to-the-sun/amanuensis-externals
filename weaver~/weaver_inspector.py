import socket
import json
import pyray as pr
import os
import wave
import struct
import sys
import ctypes

def minimize_console():
    if sys.platform == 'win32':
        try:
            hwnd = ctypes.windll.kernel32.GetConsoleWindow()
            if hwnd:
                ctypes.windll.user32.ShowWindow(hwnd, 6) # SW_MINIMIZE = 6
        except Exception:
            pass

class WaveformCache:
    def __init__(self):
        self.cache = {}

    def get_peaks(self, filepath, num_peaks=1000):
        if filepath in self.cache:
            return self.cache[filepath]

        if not os.path.exists(filepath):
            return None, 0.0

        try:
            with wave.open(filepath, 'rb') as wf:
                n_channels = wf.getnchannels()
                n_frames = wf.getnframes()
                sr = wf.getframerate()
                duration_ms = (n_frames * 1000.0) / sr

                read_size = min(n_frames, 500000) # Cap read size for fast load
                raw_bytes = wf.readframes(read_size)
                fmt = f"<{read_size * n_channels}h"
                samples = struct.unpack(fmt, raw_bytes)

                step = max(1, len(samples) // (num_peaks * n_channels))
                peaks = []
                for i in range(0, len(samples), step * n_channels):
                    chunk = samples[i:i + step * n_channels]
                    if chunk:
                        max_val = max(abs(s) for s in chunk) / 32768.0
                        peaks.append(max_val)
                    if len(peaks) >= num_peaks:
                        break

                res = (peaks, duration_ms)
                self.cache[filepath] = res
                return res
        except Exception as e:
            print(f"Error loading WAV {filepath}: {e}")
            return None, 0.0

def main():
    minimize_console()
    UDP_IP = "127.0.0.1"
    UDP_PORT = 7777 # Standard visualize port

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))
    sock.setblocking(False)

    pr.init_window(1280, 720, "Weaver~ Read Position & Waveform Inspector")
    pr.set_target_fps(60)

    waveform_cache = WaveformCache()
    tracks_data = {}

    for i in range(1, 5):
        tracks_data[i] = {
            "palette": "-",
            "offset": 0.0,
            "song_ms": 0.0,
            "src_ms": 0.0,
            "f_low": 0,
            "n_frames": 0,
            "in_bounds": 1,
            "f1": 0.0,
            "f2": 0.0,
            "busy": 0
        }

    while not pr.window_should_close():
        # 1. Drain UDP Queue
        while True:
            try:
                data, addr = sock.recvfrom(4096)
                msg_str = data.decode('utf-8', errors='ignore')
                pkt = json.loads(msg_str)

                tr_id = pkt.get("track", 0)
                if tr_id in tracks_data:
                    tr = tracks_data[tr_id]
                    if "palette" in pkt:
                        tr["palette"] = pkt.get("palette", "-")
                    if "offset" in pkt:
                        tr["offset"] = float(pkt.get("offset", 0.0))
                    if "ms" in pkt:
                        tr["song_ms"] = float(pkt.get("ms", 0.0))
                    if "src_ms" in pkt:
                        tr["src_ms"] = float(pkt.get("src_ms", 0.0))
                    if "f_low" in pkt:
                        tr["f_low"] = int(pkt.get("f_low", 0))
                    if "n_frames" in pkt:
                        tr["n_frames"] = int(pkt.get("n_frames", 0))
                    if "in_bounds" in pkt:
                        tr["in_bounds"] = int(pkt.get("in_bounds", 1))
                    if "f1" in pkt:
                        tr["f1"] = float(pkt.get("f1", 0.0))
                    if "f2" in pkt:
                        tr["f2"] = float(pkt.get("f2", 0.0))
                    if "busy" in pkt:
                        tr["busy"] = int(pkt.get("busy", 0))
            except Exception:
                break

        # 2. Render Window
        pr.begin_drawing()
        pr.clear_background(pr.Color(18, 18, 24, 255))

        # Title Header
        pr.draw_text("WEAVER~ REAL-TIME SAMPLE READ INSPECTOR", 20, 15, 20, pr.WHITE)
        pr.draw_text("UDP Port: 7777 | @visualize 1 Required", 1000, 18, 14, pr.GRAY)

        lane_h = 150
        start_y = 60

        for t_id in range(1, 5):
            tr = tracks_data[t_id]
            y = start_y + (t_id - 1) * (lane_h + 10)

            # Background Box
            bg_col = pr.Color(28, 28, 38, 255) if tr["in_bounds"] else pr.Color(60, 20, 20, 255)
            pr.draw_rectangle(20, y, 1240, lane_h, bg_col)
            pr.draw_rectangle_lines(20, y, 1240, lane_h, pr.DARKGRAY)

            # Equation HUD
            eq_str = f"Track {t_id} | Palette: {tr['palette']} | Eq: src_ms = offset({tr['offset']:.1f}) + song_ms({tr['song_ms']:.1f}) = {tr['src_ms']:.1f}ms"
            pr.draw_text(eq_str, 30, y + 10, 14, pr.GREEN if tr['in_bounds'] else pr.RED)

            status_str = f"Frame: {tr['f_low']} / {tr['n_frames']} | Status: {'[VALID READ]' if tr['in_bounds'] else '[OUT OF BOUNDS - SILENT]'}"
            pr.draw_text(status_str, 30, y + 30, 14, pr.LIGHTGRAY if tr['in_bounds'] else pr.RED)

            # Waveform Display area
            wf_x = 30
            wf_y = y + 55
            wf_w = 1220
            wf_h = 80

            pr.draw_rectangle(wf_x, wf_y, wf_w, wf_h, pr.Color(10, 10, 15, 255))

            # Attempt waveform load if palette exists on disk
            pal_name = tr['palette']
            if pal_name != "-" and pal_name != "":
                wav_path = pal_name if pal_name.endswith('.wav') else pal_name + '.wav'
                peaks, duration_ms = waveform_cache.get_peaks(wav_path, num_peaks=wf_w)

                if peaks:
                    mid_y = wf_y + wf_h // 2
                    for i, p in enumerate(peaks):
                        px = wf_x + i
                        ph = int(p * (wf_h / 2))
                        pr.draw_line(px, mid_y - ph, px, mid_y + ph, pr.DARKBLUE)

                    # Playhead / Read Cursor
                    if duration_ms > 0:
                        cursor_frac = tr['src_ms'] / duration_ms
                        cursor_px = wf_x + int(cursor_frac * wf_w)

                        if wf_x <= cursor_px <= wf_x + wf_w:
                            cursor_col = pr.LIME if tr['in_bounds'] else pr.RED
                            pr.draw_line(cursor_px, wf_y, cursor_px, wf_y + wf_h, cursor_col)
                            pr.draw_circle(cursor_px, wf_y + 5, 4, cursor_col)
                else:
                    pr.draw_text(f"Waveform unavailable ({wav_path})", wf_x + 10, wf_y + 30, 14, pr.GRAY)

            # Crossfade Ramp Indicator
            pr.draw_rectangle(wf_x, wf_y + wf_h - 4, int(tr['f1'] * wf_w), 4, pr.YELLOW)

        pr.end_drawing()

    pr.close_window()

if __name__ == '__main__':
    main()

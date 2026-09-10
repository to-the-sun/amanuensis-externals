import socket
import threading
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

# Configuration
TCP_PORT = 8999

# Shared state guarded by lock
tracks_data = {}
for i in range(1, 5):
    tracks_data[i] = {
        "palette": "-",
        "bar_sym": "-",
        "offset": 0.0,
        "song_ms": 0.0,
        "src_ms": 0.0,
        "f_low": 0,
        "n_frames": 0,
        "in_bounds": 1,
        "dict_has_bar": 0,
        "f1": 0.0,
        "f2": 0.0,
        "busy": 0
    }
data_lock = threading.Lock()
pkt_count = 0

class WaveformCache:
    def __init__(self):
        self.cache = {}

    def resolve_path(self, palette_name):
        if not palette_name or palette_name == "-":
            return None

        candidates = [
            palette_name,
            palette_name + ".wav" if not palette_name.endswith(".wav") else palette_name,
            f"palette_{palette_name}" if not palette_name.startswith("palette_") else palette_name,
            f"palette_{palette_name}.wav" if not palette_name.startswith("palette_") else palette_name + ".wav",
            os.path.join("..", palette_name),
            os.path.join("..", palette_name + ".wav"),
            os.path.join("test", palette_name),
            os.path.join("test", palette_name + ".wav")
        ]

        for cand in candidates:
            if os.path.exists(cand):
                return cand
        return None

    def get_peaks(self, palette_name, num_peaks=1200):
        resolved = self.resolve_path(palette_name)
        if not resolved:
            return None, 0.0

        if resolved in self.cache:
            return self.cache[resolved]

        try:
            with wave.open(resolved, 'rb') as wf:
                n_channels = wf.getnchannels()
                n_frames = wf.getnframes()
                sr = wf.getframerate()
                duration_ms = (n_frames * 1000.0) / sr

                read_size = min(n_frames, 500000)
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
                self.cache[resolved] = res
                return res
        except Exception as e:
            print(f"[Inspector] Error reading WAV {resolved}: {e}")
            return None, 0.0

def process_line(line):
    global pkt_count
    if not line:
        return
    try:
        pkt = json.loads(line)
        pkt_type = pkt.get("type", "")

        if pkt_type == "weaver" or "track" in pkt or "src_ms" in pkt:
            tr_id = pkt.get("track", 0)
            with data_lock:
                if tr_id in tracks_data:
                    tr = tracks_data[tr_id]
                    if "palette" in pkt:
                        tr["palette"] = pkt.get("palette", "-")
                    if "bar" in pkt:
                        tr["bar_sym"] = pkt.get("bar", "-")
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
                    if "dict_has_bar" in pkt:
                        tr["dict_has_bar"] = int(pkt.get("dict_has_bar", 0))
                    if "f1" in pkt:
                        tr["f1"] = float(pkt.get("f1", 0.0))
                    if "f2" in pkt:
                        tr["f2"] = float(pkt.get("f2", 0.0))
                    if "busy" in pkt:
                        tr["busy"] = int(pkt.get("busy", 0))

                    pkt_count += 1
                    if pkt_count % 30 == 1:
                        print(f"[Inspector LOG] Packet #{pkt_count} | Track {tr_id}: palette={tr['palette']}, bar={tr['bar_sym']}, dict_has_bar={tr['dict_has_bar']}, src_ms={tr['src_ms']:.1f}ms, in_bounds={tr['in_bounds']}")
                        sys.stdout.flush()
    except json.JSONDecodeError:
        pass

def handle_client(sock, addr):
    print(f"[Inspector] Client connected from {addr}")
    sys.stdout.flush()
    buffer = ""
    while True:
        try:
            data = sock.recv(4096)
            if not data:
                break
            text = data.decode("utf-8", errors="replace")
            buffer += text
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                process_line(line.strip())
        except Exception:
            break
    sock.close()
    print(f"[Inspector] Client disconnected from {addr}")
    sys.stdout.flush()

def tcp_server():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("", TCP_PORT))
    except Exception as e:
        print(f"\n[Inspector ERROR] Failed to bind to TCP port {TCP_PORT}: {e}")
        print("[Inspector ERROR] Make sure debug_visualizer.py is NOT running on port 8999 at the same time!\n")
        sys.stdout.flush()
        return

    server_sock.listen(5)
    print(f"[Inspector SUCCESS] Listening on TCP port {TCP_PORT} for weaver~ telemetry...")
    sys.stdout.flush()

    while True:
        try:
            client_sock, addr = server_sock.accept()
            threading.Thread(target=handle_client, args=(client_sock, addr), daemon=True).start()
        except Exception as e:
            print(f"[Inspector ERROR] TCP Server accept error: {e}")
            sys.stdout.flush()

def load_clean_font():
    font_candidates = [
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf"
    ]
    for font_path in font_candidates:
        if os.path.exists(font_path):
            try:
                font = pr.load_font_ex(font_path, 32, None, 0)
                print(f"[Inspector] Successfully loaded clean TTF font: {font_path}")
                sys.stdout.flush()
                return font
            except Exception as e:
                print(f"[Inspector] Error loading font {font_path}: {e}")
    return pr.get_font_default()

def draw_text(font, text, x, y, size, color):
    if font and font.texture.id != pr.get_font_default().texture.id:
        pos = pr.Vector2(float(x), float(y))
        pr.draw_text_ex(font, text, pos, float(size), 1.0, color)
    else:
        pr.draw_text(text, int(x), int(y), int(size), color)

def main():
    minimize_console()
    print("[Inspector STARTUP] Initializing Weaver Read-Position Inspector...")
    sys.stdout.flush()

    threading.Thread(target=tcp_server, daemon=True).start()

    pr.init_window(1280, 720, "Weaver~ Read Position & Waveform Inspector")
    pr.set_target_fps(60)

    clean_font = load_clean_font()
    waveform_cache = WaveformCache()

    # Dark high-contrast palette
    c_bg = pr.Color(16, 18, 22, 255)
    c_card_ok = pr.Color(24, 28, 36, 255)
    c_card_err = pr.Color(50, 20, 24, 255)
    c_border = pr.Color(45, 52, 65, 255)
    c_green = pr.Color(50, 215, 120, 255)
    c_red = pr.Color(245, 80, 80, 255)
    c_gray = pr.Color(160, 170, 185, 255)

    while not pr.window_should_close():
        pr.begin_drawing()
        pr.clear_background(c_bg)

        # Header Title
        draw_text(clean_font, "WEAVER~ REAL-TIME READ-POSITION INSPECTOR", 20, 14, 20, pr.WHITE)
        draw_text(clean_font, f"TCP Port: {TCP_PORT}  |  Packets: {pkt_count}", 940, 16, 15, pr.YELLOW if pkt_count > 0 else c_gray)

        lane_h = 150
        start_y = 55

        with data_lock:
            snap_tracks = {i: tracks_data[i].copy() for i in range(1, 5)}

        for t_id in range(1, 5):
            tr = snap_tracks[t_id]
            y = start_y + (t_id - 1) * (lane_h + 10)

            # Lane background box
            bg_col = c_card_ok if tr["in_bounds"] else c_card_err
            pr.draw_rectangle(20, y, 1240, lane_h, bg_col)
            pr.draw_rectangle_lines(20, y, 1240, lane_h, c_border)

            # 1. Transcript Bar Status Badge
            bar_sym = tr['bar_sym'] if tr['bar_sym'] != "" else "-"
            if tr['dict_has_bar']:
                badge_bg = pr.Color(30, 80, 50, 255)
                badge_fg = pr.Color(120, 255, 160, 255)
                badge_txt = f"TRANSCRIPT BAR: {bar_sym}"
            else:
                badge_bg = pr.Color(45, 50, 60, 255)
                badge_fg = pr.Color(160, 165, 180, 255)
                badge_txt = f"NO BAR IN TRANSCRIPT ({bar_sym})"

            pr.draw_rectangle(30, y + 10, 210, 24, badge_bg)
            pr.draw_rectangle_lines(30, y + 10, 210, 24, badge_fg)
            draw_text(clean_font, badge_txt, 38, y + 15, 13, badge_fg)

            # 2. Live Equation HUD Overlay
            pal_display = tr['palette']
            eq_txt = f"Track {t_id} | Palette: {pal_display}  |  Eq: src_ms = offset({tr['offset']:.1f}) + song_ms({tr['song_ms']:.1f}) = {tr['src_ms']:.1f} ms"
            draw_text(clean_font, eq_txt, 255, y + 15, 14, c_green if tr['in_bounds'] else c_red)

            status_txt = f"Frame: {tr['f_low']} / {tr['n_frames']}  |  Read Status: {'[VALID READ]' if tr['in_bounds'] else '[OUT OF BOUNDS - SILENT]'}"
            draw_text(clean_font, status_txt, 30, y + 38, 13, pr.LIGHTGRAY if tr['in_bounds'] else c_red)

            # 3. Waveform Canvas Area
            wf_x = 30
            wf_y = y + 58
            wf_w = 1220
            wf_h = 78

            pr.draw_rectangle(wf_x, wf_y, wf_w, wf_h, pr.Color(10, 12, 16, 255))
            pr.draw_rectangle_lines(wf_x, wf_y, wf_w, wf_h, pr.Color(35, 40, 50, 255))

            peaks, duration_ms = waveform_cache.get_peaks(pal_display, num_peaks=wf_w)

            if peaks:
                mid_y = wf_y + wf_h // 2
                for i, p in enumerate(peaks):
                    px = wf_x + i
                    ph = int(p * (wf_h / 2))
                    pr.draw_line(px, mid_y - ph, px, mid_y + ph, pr.Color(70, 130, 200, 255))

                # Playhead Read Cursor
                if duration_ms > 0:
                    cursor_frac = tr['src_ms'] / duration_ms
                    cursor_px = wf_x + int(cursor_frac * wf_w)

                    if wf_x <= cursor_px <= wf_x + wf_w:
                        cursor_col = pr.LIME if tr['in_bounds'] else c_red
                        pr.draw_line(cursor_px, wf_y, cursor_px, wf_y + wf_h, cursor_col)
                        pr.draw_circle(cursor_px, wf_y + 5, 4, cursor_col)
            else:
                # Max Buffer Virtual View (Rendered when WAV is in Max memory buffer~ rather than disk)
                n_fr = tr['n_frames']
                if n_fr > 0:
                    draw_text(clean_font, f"Max Buffer~: {pal_display} ({n_fr} frames)", wf_x + 15, wf_y + 15, 14, pr.SKYBLUE)
                    mid_y = wf_y + wf_h // 2
                    pr.draw_line(wf_x, mid_y, wf_x + wf_w, mid_y, pr.DARKBLUE)

                    # Virtual Read Cursor
                    cursor_frac = tr['f_low'] / float(n_fr)
                    cursor_px = wf_x + int(cursor_frac * wf_w)
                    if wf_x <= cursor_px <= wf_x + wf_w:
                        cursor_col = pr.LIME if tr['in_bounds'] else c_red
                        pr.draw_line(cursor_px, wf_y, cursor_px, wf_y + wf_h, cursor_col)
                        pr.draw_circle(cursor_px, wf_y + 5, 4, cursor_col)
                else:
                    draw_text(clean_font, f"Buffer / Track Inactive ({pal_display})", wf_x + 15, wf_y + 30, 14, pr.DARKGRAY)

            # Crossfade Ramp Indicator Bar
            pr.draw_rectangle(wf_x, wf_y + wf_h - 4, int(tr['f1'] * wf_w), 4, pr.YELLOW)

        pr.end_drawing()

    if clean_font and clean_font.texture.id != pr.get_font_default().texture.id:
        pr.unload_font(clean_font)

    pr.close_window()

if __name__ == '__main__':
    main()

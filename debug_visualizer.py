import pygame
import threading
import socket
import json
import time
import math
import os
import sys
import hashlib

# Set dummy video driver for headless environments
if os.environ.get('HEADLESS'):
    os.environ['SDL_VIDEODRIVER'] = 'dummy'
    os.environ['SDL_AUDIODRIVER'] = 'dummy'

# Configuration
TCP_PORT = 8999
SCALE = 0.8
WINDOW_SIZE = (int(1200 * SCALE), int(1000 * SCALE))
FPS = 60

# State (shared between threads; guarded by lock)
state = {
    # Building State
    "palettes": {},
    "bar_length": 125.0,
    "current_offset": 0.0,
    "loop_start": 0.0,

    # Weaver State
    "data_points_by_track": {},
    "labels_by_track": {},
    "track_lengths": {},
    "busy_states": {},
    "tracks_seen": {1, 2, 3, 4},
    "global_min_ms": 0.0,
    "global_max_ms": 0.0,
    "main_ramp_duration": 5000.0,

    "error_message": None
}
state_lock = threading.Lock()

def get_color_for_label(label_text):
    if not label_text or label_text.startswith("-@"):
        return (45, 45, 52, 120)

    # Generate a stable color based on the palette@offset string
    h = hashlib.md5(label_text.encode()).digest()
    r = 60 + (h[0] % 80)
    g = 60 + (h[1] % 80)
    b = 80 + (h[2] % 80)
    return (r, g, b, 80)

def process_packet(text):
    if not text:
        return

    # Handle multiple JSON objects that might be in the text
    text = text.replace("} {", "}\n{").replace("}{", "}\n{")

    for line in text.split('\n'):
        line = line.strip()
        if not line:
            continue

        start = line.find('{')
        end = line.rfind('}')
        if start == -1 or end == -1: continue
        line = line[start:end+1]

        try:
            pkt = json.loads(line)
            pkt_type = pkt.get("type")

            with state_lock:
                if pkt_type == "weaver":
                    # 1. Weaver 'clear'
                    if "clear" in pkt:
                        if state["global_max_ms"] > state["global_min_ms"]:
                            state["main_ramp_duration"] = state["global_max_ms"] - state["global_min_ms"]
                        state["data_points_by_track"].clear()
                        state["labels_by_track"].clear()
                        state["track_lengths"].clear()
                        state["busy_states"].clear()
                        state["tracks_seen"] = {1, 2, 3, 4}
                        state["global_min_ms"] = 0.0
                        state["global_max_ms"] = 0.0
                        print(f"!!! Weaver state cleared via TCP. New scale: {state['main_ramp_duration']:.0f}ms !!!")
                        sys.stdout.flush()
                        continue

                    # 2. Weaver Labels
                    if all(k in pkt for k in ["track", "ms", "palette", "offset"]):
                        track_id = int(pkt["track"])
                        label_text = f"{pkt['palette']}@{pkt['offset']:.0f}"
                        if track_id not in state["labels_by_track"]:
                            state["labels_by_track"][track_id] = []
                        pos_idx = len(state["labels_by_track"][track_id]) % 2
                        state["labels_by_track"][track_id].append({
                            "ms": pkt["ms"],
                            "text": label_text,
                            "bar": pkt.get("bar", ""),
                            "len": pkt.get("len", 0),
                            "pos_idx": pos_idx
                        })
                        if "len" in pkt:
                            state["track_lengths"][track_id] = float(pkt["len"])
                        if "busy" in pkt:
                            state["busy_states"][track_id] = bool(pkt["busy"])
                        state["tracks_seen"].add(track_id)
                        if pkt["ms"] < state["global_min_ms"]:
                            state["global_min_ms"] = pkt["ms"]
                        if pkt["ms"] > state["global_max_ms"]:
                            state["global_max_ms"] = pkt["ms"]
                        if len(state["labels_by_track"][track_id]) > 1000:
                            state["labels_by_track"][track_id].pop(0)

                    # 3. Weaver Data Points
                    elif all(k in pkt for k in ["track", "ms", "f1", "f2"]):
                        track_id = int(pkt["track"])
                        if track_id not in state["data_points_by_track"]:
                            state["data_points_by_track"][track_id] = []
                        state["data_points_by_track"][track_id].append(pkt)
                        if "len" in pkt:
                            state["track_lengths"][track_id] = float(pkt["len"])
                        if "busy" in pkt:
                            state["busy_states"][track_id] = bool(pkt["busy"])
                        state["tracks_seen"].add(track_id)
                        if pkt["ms"] < state["global_min_ms"]:
                            state["global_min_ms"] = pkt["ms"]
                        if pkt["ms"] > state["global_max_ms"]:
                            state["global_max_ms"] = pkt["ms"]
                        if len(state["data_points_by_track"][track_id]) > 10000:
                            state["data_points_by_track"][track_id].pop(0)

                elif pkt_type == "building":
                    if "palettes" in pkt:
                        state["palettes"] = pkt["palettes"]
                    if "bar_length" in pkt:
                        state["bar_length"] = float(pkt["bar_length"])
                    if "current_offset" in pkt:
                        state["current_offset"] = float(pkt["current_offset"])
                    if "loop_start" in pkt:
                        state["loop_start"] = float(pkt["loop_start"])
                    if "building" in pkt and "palettes" not in pkt:
                        state["palettes"] = {"default": {"building": pkt["building"]}}

        except json.JSONDecodeError as e:
            print(f"JSON parse error: {e}")
            continue

def tcp_server():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("", TCP_PORT))
    except Exception as e:
        print(f"ERROR: Failed to bind to port {TCP_PORT}: {e}")
        sys.exit(1)
    server_sock.listen(5)
    print(f"Visualizer: Listening on TCP port {TCP_PORT}")
    sys.stdout.flush()
    while True:
        try:
            client_sock, addr = server_sock.accept()
            print(f"Accepted connection from {addr}")
            sys.stdout.flush()
            threading.Thread(target=handle_client, args=(client_sock,), daemon=True).start()
        except Exception as e:
            print(f"TCP Server error: {e}")
            sys.stdout.flush()

def handle_client(sock):
    buffer = ""
    while True:
        try:
            data = sock.recv(4096)
            if not data: break
            text = data.decode("utf-8", errors="replace")
            buffer += text
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                process_packet(line.strip())
        except Exception: break
    sock.close()

def draw_building(surface, palettes, bar_length, current_offset, loop_start, fonts):
    w, h = surface.get_size()
    surface.fill((38, 38, 46)) # BACKGROUND_BUILDING

    if not palettes:
        return

    num_palettes = len(palettes)
    palette_h = (h - int(60 * SCALE)) / num_palettes # margin for labels

    grid_left = int(60 * SCALE)
    grid_right = w - int(40 * SCALE)
    grid_w = grid_right - grid_left

    sorted_palettes = sorted(palettes.keys())

    for p_idx, p_name in enumerate(sorted_palettes):
        p_data = palettes[p_name]
        working_memory = p_data.get("building", {})

        p_top = int(40 * SCALE) + p_idx * palette_h
        p_timeline_h = palette_h * 0.8

        # Palette Label
        p_label = fonts["building_large"].render(f"Palette: {p_name}", True, (255, 255, 255))
        surface.blit(p_label, (grid_left, p_top - int(25 * SCALE)))

        # Display loop_start in the corner
        ls_label = fonts["building_normal"].render(f"loop_start: {loop_start:.2f}", True, (150, 150, 150))
        surface.blit(ls_label, (w - ls_label.get_width() - int(10 * SCALE), p_top - int(25 * SCALE)))

        # draw working_memory timeline
        all_ts = [
            ts
            for track_data in working_memory.values()
            for key, ts_list in track_data.items()
            if key in ("absolutes", "offsets")
            for ts in ts_list
        ]
        if current_offset is not None:
            all_ts.append(current_offset)

        if all_ts:
            min_ts, max_ts = min(all_ts), max(all_ts)
            span_ts = max_ts - min_ts if max_ts > min_ts else 1.0

            timeline_rect = pygame.Rect(grid_left, p_top, grid_w, p_timeline_h)
            pygame.draw.rect(surface, (20, 20, 25), timeline_rect)

            # Draw min and max labels for the timeline
            min_label = fonts["building_small"].render(f"{min_ts:.2f}", True, (204, 204, 204))
            max_label = fonts["building_small"].render(f"{max_ts:.2f}", True, (204, 204, 204))
            surface.blit(min_label, (grid_left, p_top + p_timeline_h + int(5 * SCALE)))
            surface.blit(max_label, (grid_right - max_label.get_width(), p_top + p_timeline_h + int(5 * SCALE)))

            if working_memory:
                # Each unique track-offset identifier gets its own row
                sorted_track_keys = sorted(working_memory.keys(), key=lambda k: int(k.split('-')[0]))
                track_h = p_timeline_h / max(1, len(sorted_track_keys))

                for i, track_id in enumerate(sorted_track_keys):
                    track_data = working_memory[track_id]
                    track_y = p_top + i * track_h

                    # Draw track label
                    track_label = fonts["building_normal"].render(f"T {track_id}", True, (204, 204, 204))
                    surface.blit(track_label, (int(5 * SCALE), track_y + track_h / 2 - track_label.get_height() / 2))

                    # Draw horizontal bars for spans (drawn first, in the background)
                    span_data = track_data.get("span", [])
                    if span_data:
                        try:
                            offset_val = float(track_id.split('-')[1])
                            # Subtract loop_start for visual position calculation only
                            min_abs_span_ts = (min(span_data) - loop_start) + offset_val
                            max_abs_span_ts = (max(span_data) - loop_start) + offset_val + bar_length

                            start_x = grid_left + grid_w * (min_abs_span_ts - min_ts) / span_ts
                            end_x = grid_left + grid_w * (max_abs_span_ts - min_ts) / span_ts

                            bar_y = track_y + track_h * 0.5
                            bar_height = track_h * 0.4

                            s = pygame.Surface((max(1, end_x - start_x), bar_height), pygame.SRCALPHA)
                            s.fill((60, 60, 100, 128))
                            surface.blit(s, (start_x, bar_y - bar_height / 2))

                            for bar_relative_ts in span_data:
                                # Subtract loop_start for visual position calculation only
                                bar_abs_start_ts = (bar_relative_ts - loop_start) + offset_val
                                bar_start_x = grid_left + grid_w * (bar_abs_start_ts - min_ts) / span_ts
                                bar_width_pixels = (grid_w * bar_length) / span_ts

                                s = pygame.Surface((max(1, bar_width_pixels), bar_height), pygame.SRCALPHA)
                                s.fill((90, 90, 130, 128))
                                surface.blit(s, (bar_start_x, bar_y - bar_height / 2))

                                # Display relative value directly
                                label_text = f"{bar_relative_ts:.0f}"
                                label = fonts["building_small"].render(label_text, True, (204, 204, 204))
                                surface.blit(label, (bar_start_x + int(2 * SCALE), bar_y - bar_height / 2 - int(15 * SCALE)))

                        except (ValueError, IndexError): pass

                    # Draw hash marks for absolutes
                    for ts in track_data.get("absolutes", []):
                        x = grid_left + grid_w * (ts - min_ts) / span_ts
                        pygame.draw.line(surface, (100, 200, 100), (x, track_y), (x, track_y + track_h), 1)
                        label = fonts["building_small"].render(f"{ts:.2f}", True, (100, 200, 100))
                        surface.blit(label, (x + int(2 * SCALE), track_y + int(5 * SCALE)))

                palette_offsets = set()
                for track_data in working_memory.values():
                    palette_offsets.update(track_data.get("offsets", []))

                for i, track_id in enumerate(sorted_track_keys):
                    track_y = p_top + i * track_h
                    for ts in palette_offsets:
                        x = grid_left + grid_w * (ts - min_ts) / span_ts
                        pygame.draw.line(surface, (200, 100, 100), (x, track_y), (x, track_y + track_h), 2)
                        if i == 0:
                            label = fonts["building_small"].render(f"{ts:.0f}", True, (200, 100, 100))
                            surface.blit(label, (x + int(2 * SCALE), p_top - int(15 * SCALE)))

def draw_weaver(surface, points_dict, labels_dict, busy_dict, tracks, view_start_ms, view_end_ms, fonts):
    w, h = surface.get_size()
    surface.fill((30, 30, 35)) # BACKGROUND_WEAVER

    view_span_ms = view_end_ms - view_start_ms
    if view_span_ms <= 0:
        view_span_ms = 1.0

    total_points = sum(len(pts) for pts in points_dict.values())
    status_text = fonts["weaver_status"].render(f"Tracks: {len(tracks)} Points: {total_points} Duration: {view_span_ms:.0f}ms [Press 'C' to clear]", True, (150, 150, 150))
    surface.blit(status_text, (w - status_text.get_width() - int(20 * SCALE), int(20 * SCALE)))

    if not tracks:
        return

    left_pad, right_pad, top_pad, bottom_pad = int(80 * SCALE), int(50 * SCALE), int(60 * SCALE), int(60 * SCALE)
    row_spacing = int(20 * SCALE)
    graph_w = w - left_pad - right_pad
    graph_h = h - top_pad - bottom_pad

    num_rows = max(tracks) if tracks else 1
    row_full_h = graph_h / num_rows
    row_graph_h = row_full_h - row_spacing

    for t in range(1, num_rows + 1):
        i = t - 1
        y_graph = top_pad + i * row_full_h + row_spacing / 2
        if i % 2 == 0:
            pygame.draw.rect(surface, (35, 35, 42), (left_pad, y_graph, graph_w, row_graph_h))

    # Draw shaded backgrounds for palette/offset intervals
    for t in tracks:
        i = t - 1
        row_top = top_pad + i * row_full_h + row_spacing / 2
        track_labels = sorted(labels_dict.get(t, []), key=lambda l: l["ms"])
        if not track_labels: continue

        for j, l in enumerate(track_labels):
            start_ms = l["ms"]
            if j + 1 < len(track_labels):
                end_ms = track_labels[j+1]["ms"]
            else:
                end_ms = view_end_ms

            if start_ms >= view_end_ms: continue

            lx = left_pad + ((start_ms - view_start_ms) / view_span_ms) * graph_w
            rx = left_pad + ((min(end_ms, view_end_ms) - view_start_ms) / view_span_ms) * graph_w

            width = int(rx) - int(lx)
            if width > 0:
                color = get_color_for_label(l["text"])
                s = pygame.Surface((width, int(row_graph_h)), pygame.SRCALPHA)
                s.fill(color)
                surface.blit(s, (int(lx), int(row_top)))

    for t in tracks:
        i = t - 1
        row_top = top_pad + i * row_full_h + row_spacing / 2
        row_bottom = row_top + row_graph_h
        track_points = points_dict.get(t, [])
        if len(track_points) < 2: continue
        f1_pts, f2_pts = [], []
        for p in track_points:
            x = left_pad + ((p["ms"] - view_start_ms) / view_span_ms) * graph_w
            y_f1 = row_bottom - p["f1"] * row_graph_h
            y_f2 = row_bottom - p["f2"] * row_graph_h
            f1_pts.append((int(x), int(y_f1)))
            f2_pts.append((int(x), int(y_f2)))
        if len(f1_pts) >= 2: pygame.draw.lines(surface, (100, 255, 100), False, f1_pts, 2)
        if len(f2_pts) >= 2: pygame.draw.lines(surface, (255, 100, 100), False, f2_pts, 2)

    for t in tracks:
        i = t - 1
        row_top = top_pad + i * row_full_h + row_spacing / 2
        row_bottom = row_top + row_graph_h
        track_labels = labels_dict.get(t, [])
        for l in track_labels:
            lx = left_pad + ((l["ms"] - view_start_ms) / view_span_ms) * graph_w
            if left_pad <= lx <= left_pad + graph_w:
                pygame.draw.line(surface, (100, 100, 120), (int(lx), row_top), (int(lx), row_bottom), 1)
                txt_bar = fonts["weaver_tiny"].render(f"{l.get('bar', '')}", True, (220, 220, 255))
                txt_label = fonts["weaver_tiny"].render(l["text"], True, (180, 180, 200))
                if l.get("pos_idx", 0) == 0:
                    ty1, ty2 = row_top + int(2 * SCALE), row_top + int(13 * SCALE)
                else:
                    ty2, ty1 = row_bottom - int(12 * SCALE), row_bottom - int(23 * SCALE)
                surface.blit(txt_bar, (int(lx) + int(3 * SCALE), ty1))
                surface.blit(txt_label, (int(lx) + int(3 * SCALE), ty2))

    for t in range(1, num_rows + 1):
        i = t - 1
        y_graph = top_pad + i * row_full_h + row_spacing / 2
        pygame.draw.line(surface, (60, 60, 70), (left_pad, y_graph), (left_pad + graph_w, y_graph), 1)
        pygame.draw.line(surface, (60, 60, 70), (left_pad, y_graph + row_graph_h), (left_pad + graph_w, y_graph + row_graph_h), 1)

        is_busy = busy_dict.get(t, False)
        t_len = state["track_lengths"].get(t, 0)

        if is_busy:
            pygame.draw.rect(surface, (255, 255, 255), (int(5 * SCALE), y_graph + row_graph_h/2 - int(15 * SCALE), int(70 * SCALE), int(30 * SCALE)))
            lbl = fonts["weaver_label"].render(f"Track {t}", True, (0, 0, 0))
            lbl_len = fonts["weaver_tiny"].render(f"L: {t_len:.0f}ms", True, (0, 0, 0))
        else:
            lbl = fonts["weaver_label"].render(f"Track {t}", True, (200, 200, 200))
            lbl_len = fonts["weaver_tiny"].render(f"L: {t_len:.0f}ms", True, (150, 150, 150))

        surface.blit(lbl, (int(10 * SCALE), y_graph + row_graph_h/2 - int(12 * SCALE)))
        surface.blit(lbl_len, (int(10 * SCALE), y_graph + row_graph_h/2 + int(3 * SCALE)))

    axis_y = top_pad + graph_h
    pygame.draw.line(surface, (200, 200, 200), (left_pad, axis_y), (left_pad + graph_w, axis_y), 2)
    for i in range(11):
        tick_ms = view_start_ms + (i / 10) * view_span_ms
        tick_x = left_pad + (i / 10) * graph_w
        pygame.draw.line(surface, (200, 200, 200), (int(tick_x), axis_y), (int(tick_x), axis_y + int(5 * SCALE)), 2)
        tick_label = fonts["weaver_tiny"].render(f"{tick_ms:.0f} ms", True, (200, 200, 200))
        surface.blit(tick_label, (int(tick_x) - tick_label.get_width()/2, axis_y + int(10 * SCALE)))

def run_gui():
    os.environ['SDL_VIDEO_WINDOW_POS'] = "960,0"
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Combined Visualizer")
    clock = pygame.time.Clock()

    try:
        fonts = {
            "building_normal": pygame.font.SysFont("Arial", int(16 * SCALE)),
            "building_large": pygame.font.SysFont("Arial", int(20 * SCALE)),
            "building_small": pygame.font.SysFont("Arial", int(12 * SCALE)),
            "weaver_tiny": pygame.font.SysFont("Arial", int(10 * SCALE)),
            "weaver_label": pygame.font.SysFont("Arial", int(14 * SCALE)),
            "weaver_status": pygame.font.SysFont("Arial", int(16 * SCALE))
        }
    except:
        default_font = pygame.font.Font(None, int(20 * SCALE))
        fonts = {k: default_font for k in ["building_normal", "building_large", "building_small", "weaver_tiny", "weaver_label", "weaver_status"]}

    while True:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit(); sys.exit()
            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_c:
                    with state_lock:
                        state["data_points_by_track"].clear()
                        state["labels_by_track"].clear()
                        state["tracks_seen"] = {1, 2, 3, 4}
                        state["global_min_ms"] = 0.0
                        state["global_max_ms"] = 0.0

        with state_lock:
            # Snapshots
            p_palettes = state["palettes"].copy()
            p_bar_len = state["bar_length"]
            p_offset = state["current_offset"]
            p_loop_start = state["loop_start"]
            p_points = {tid: list(pts) for tid, pts in state["data_points_by_track"].items()}
            p_labels = {tid: list(lbs) for tid, lbs in state["labels_by_track"].items()}
            p_busy = state["busy_states"].copy()
            p_tracks = sorted(list(state["tracks_seen"]))
            p_min_ms = state["global_min_ms"]
            p_max_ms = state["global_max_ms"]
            p_ramp_dur = state["main_ramp_duration"]

        building_surf = screen.subsurface((0, 0, int(1200 * SCALE), int(400 * SCALE)))
        draw_building(building_surf, p_palettes, p_bar_len, p_offset, p_loop_start, fonts)

        weaver_surf = screen.subsurface((0, int(400 * SCALE), int(1200 * SCALE), int(600 * SCALE)))
        view_start_ms = p_min_ms
        view_end_ms = max(p_min_ms + p_ramp_dur, p_max_ms)
        draw_weaver(weaver_surf, p_points, p_labels, p_busy, p_tracks, view_start_ms, view_end_ms, fonts)

        pygame.display.flip()
        clock.tick(FPS)

if __name__ == "__main__":
    threading.Thread(target=tcp_server, daemon=True).start()
    run_gui()

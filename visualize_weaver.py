import pygame
import threading
import socket
import json
import time
import math
import os
import sys

# Set dummy video driver for headless environments
if os.environ.get('HEADLESS'):
    os.environ['SDL_VIDEODRIVER'] = 'dummy'
    os.environ['SDL_AUDIODRIVER'] = 'dummy'

# Configuration
TCP_PORT = 9999
WINDOW_SIZE = (1200, 800)
BACKGROUND = (30, 30, 35)
FPS = 60

# State
# Maps track_id -> list of points
data_points_by_track = {}
# Maps track_id -> list of labels
labels_by_track = {}
# Maps track_id -> bool
busy_states = {}
tracks_seen = set()
global_max_ms = 0.0
main_ramp_duration = 5000.0 # Default fallback

state_lock = threading.Lock()

def process_text(text):
    global global_max_ms, main_ramp_duration
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
            if "clear" in pkt:
                with state_lock:
                    # Capture the max MS as the new main ramp duration before clearing
                    if global_max_ms > 0:
                        main_ramp_duration = global_max_ms

                    # Always perform global clear as requested
                    data_points_by_track.clear()
                    labels_by_track.clear()
                    busy_states.clear()
                    tracks_seen.clear()
                    global_max_ms = 0.0
                    print(f"!!! Visualizer state cleared via TCP. New scale: {main_ramp_duration:.0f}ms !!!")
                sys.stdout.flush()
                continue

            if all(k in pkt for k in ["track", "ms", "label"]):
                track_id = pkt["track"]
                with state_lock:
                    if track_id not in labels_by_track:
                        labels_by_track[track_id] = []
                    labels_by_track[track_id].append({
                        "ms": pkt["ms"],
                        "text": pkt["label"],
                        "bar": pkt.get("bar", ""),
                        "len": pkt.get("len", 0),
                        "f2": pkt.get("f2", 0.0)
                    })
                    if "busy" in pkt:
                        busy_states[track_id] = bool(pkt["busy"])
                    tracks_seen.add(track_id)
                    if len(labels_by_track[track_id]) > 1000:
                        labels_by_track[track_id].pop(0)
                continue

            valid = all(k in pkt for k in ["track", "ms", "f1", "f2"])
            if valid:
                track_id = pkt["track"]
                with state_lock:
                    if track_id not in data_points_by_track:
                        data_points_by_track[track_id] = []
                    data_points_by_track[track_id].append(pkt)
                    if "busy" in pkt:
                        busy_states[track_id] = bool(pkt["busy"])
                    tracks_seen.add(track_id)
                    if pkt["ms"] > global_max_ms:
                        global_max_ms = pkt["ms"]

                    # Keep a reasonable history per track
                    if len(data_points_by_track[track_id]) > 10000:
                        data_points_by_track[track_id].pop(0)

        except json.JSONDecodeError:
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
            if not data:
                break
            try:
                text = data.decode("utf-8", errors="replace")
                buffer += text
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    process_text(line)
            except Exception as e:
                print(f"DECODE ERROR: {e}")
                sys.stdout.flush()
        except Exception as e:
            print(f"Client handler error: {e}")
            sys.stdout.flush()
            break
    sock.close()

def run_gui():
    global global_max_ms, main_ramp_duration
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Weaver~ Crossfade Visualizer")
    clock = pygame.time.Clock()

    try:
        font = pygame.font.SysFont("Arial", 10)
        label_font = pygame.font.SysFont("Arial", 14)
        status_font = pygame.font.SysFont("Arial", 16)
    except:
        font = pygame.font.Font(None, 20)
        label_font = pygame.font.Font(None, 24)
        status_font = pygame.font.Font(None, 28)

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_c:
                    with state_lock:
                        data_points_by_track.clear()
                        labels_by_track.clear()
                        tracks_seen.clear()
                        global_max_ms = 0.0

        screen.fill(BACKGROUND)

        with state_lock:
            # Shallow copy of the structures
            points_dict = {tid: list(pts) for tid, pts in data_points_by_track.items()}
            labels_dict = {tid: list(lbs) for tid, lbs in labels_by_track.items()}
            busy_dict = dict(busy_states)
            tracks = sorted(list(tracks_seen))
            current_max_ms = global_max_ms
            current_main_ramp_duration = main_ramp_duration
            total_points = sum(len(pts) for pts in points_dict.values())

        # Status text
        status_text = status_font.render(f"Tracks: {len(tracks)} Points: {total_points} Duration: {current_main_ramp_duration:.0f}ms [Press 'C' to clear]", True, (150, 150, 150))
        screen.blit(status_text, (WINDOW_SIZE[0] - status_text.get_width() - 20, 20))

        if not tracks:
            pygame.display.flip()
            clock.tick(FPS)
            continue

        # Use the fixed duration from the last full ramp (or current max if it's larger)
        view_width_ms = max(current_main_ramp_duration, current_max_ms)
        display_min_ms = 0.0

        left_pad = 80
        right_pad = 50
        top_pad = 60
        bottom_pad = 60
        row_spacing = 20 # Buffer zone between graphs

        graph_w = WINDOW_SIZE[0] - left_pad - right_pad
        graph_h = WINDOW_SIZE[1] - top_pad - bottom_pad

        num_rows = max(tracks) if tracks else 1
        row_full_h = graph_h / num_rows
        row_graph_h = row_full_h - row_spacing

        # 1. Background rectangles for all rows
        for t in range(1, num_rows + 1):
            i = t - 1
            y_full = top_pad + i * row_full_h
            y_graph = y_full + row_spacing / 2
            if i % 2 == 0:
                pygame.draw.rect(screen, (35, 35, 42), (left_pad, y_graph, graph_w, row_graph_h))

        # 2. Colored crossfade lines (f1 and f2) - Layer 0 (Bottom)
        for t in tracks:
            i = t - 1
            y_full = top_pad + i * row_full_h
            row_top = y_full + row_spacing / 2
            row_bottom = row_top + row_graph_h

            track_points = points_dict.get(t, [])
            if len(track_points) < 2:
                continue

            f1_points = []
            f2_points = []

            for p in track_points:
                x = left_pad + (p["ms"] / view_width_ms) * graph_w
                y_f1 = row_bottom - p["f1"] * row_graph_h
                y_f2 = row_bottom - p["f2"] * row_graph_h
                f1_points.append((int(x), int(y_f1)))
                f2_points.append((int(x), int(y_f2)))

            if len(f1_points) >= 2:
                pygame.draw.lines(screen, (100, 255, 100), False, f1_points, 2) # Green for f1
            if len(f2_points) >= 2:
                pygame.draw.lines(screen, (255, 100, 100), False, f2_points, 2) # Red for f2

        # 3. Crossfade labels (vertical lines and text) - Layer 1
        for t in tracks:
            i = t - 1
            y_full = top_pad + i * row_full_h
            row_top = y_full + row_spacing / 2
            row_bottom = row_top + row_graph_h

            track_labels = labels_dict.get(t, [])
            for l in track_labels:
                lx = left_pad + (l["ms"] / view_width_ms) * graph_w
                if left_pad <= lx <= left_pad + graph_w:
                    pygame.draw.line(screen, (100, 100, 120), (int(lx), row_top), (int(lx), row_bottom), 1)

                    # Two lines:
                    # 1. Bar value / track length
                    # 2. Palette@offset (the 'text' field)
                    txt_bar = font.render(f"{l.get('bar', '')}/{l.get('len', 0)}", True, (220, 220, 255))
                    txt_label = font.render(l["text"], True, (180, 180, 200))

                    # Position based on f2 (Slot 1) state at initiation
                    # f2 at 0.0 (Slot 0 active) -> TOP
                    # f2 at 1.0 (Slot 1 active) -> BOTTOM
                    if l.get("f2", 0.0) < 0.5:
                        ty1 = row_top + 2
                        ty2 = ty1 + 11
                    else:
                        ty2 = row_bottom - 12
                        ty1 = ty2 - 11

                    screen.blit(txt_bar, (int(lx) + 3, ty1))
                    screen.blit(txt_label, (int(lx) + 3, ty2))

        # 4. Row boundary lines and Track ID labels - Layer 2
        for t in range(1, num_rows + 1):
            i = t - 1
            y_full = top_pad + i * row_full_h
            y_graph = y_full + row_spacing / 2

            # Boundary lines for the buffered row
            pygame.draw.line(screen, (60, 60, 70), (left_pad, y_graph), (left_pad + graph_w, y_graph), 1)
            pygame.draw.line(screen, (60, 60, 70), (left_pad, y_graph + row_graph_h), (left_pad + graph_w, y_graph + row_graph_h), 1)

            is_busy = busy_dict.get(t, False)
            if is_busy:
                # White background for busy tracks
                pygame.draw.rect(screen, (255, 255, 255), (5, y_graph + row_graph_h/2 - 10, 70, 20))
                label = label_font.render(f"Track {t}", True, (0, 0, 0))
            else:
                label = label_font.render(f"Track {t}", True, (200, 200, 200))
            screen.blit(label, (10, y_graph + row_graph_h/2 - 7))

        # 5. Main time axis and status text - Layer 3 (Top)
        axis_y = top_pad + graph_h
        pygame.draw.line(screen, (200, 200, 200), (left_pad, axis_y), (left_pad + graph_w, axis_y), 2)
        num_ticks = 10
        for i in range(num_ticks + 1):
            tick_ms = display_min_ms + (i / num_ticks) * view_width_ms
            tick_x = left_pad + (i / num_ticks) * graph_w
            pygame.draw.line(screen, (200, 200, 200), (int(tick_x), axis_y), (int(tick_x), axis_y + 5), 2)
            tick_label = font.render(f"{tick_ms:.0f} ms", True, (200, 200, 200))
            screen.blit(tick_label, (int(tick_x) - tick_label.get_width()/2, axis_y + 10))

        pygame.display.flip()
        clock.tick(FPS)

    pygame.quit()

if __name__ == "__main__":
    listener_thread = threading.Thread(target=tcp_server, daemon=True)
    listener_thread.start()
    run_gui()

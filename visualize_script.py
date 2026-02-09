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
UDP_PORT = 9999
WINDOW_SIZE = (1200, 800)
BACKGROUND = (30, 30, 35)
FPS = 60

# State
data_points = [] # list of {"track": T, "ms": M, "chan": C, "val": V, "num_chans": N}
track_chans = {} # track -> max_num_chans seen
global_min_ms = 0.0
global_max_ms = 1000.0
first_point_received = False

state_lock = threading.Lock()

def process_text(text):
    global global_min_ms, global_max_ms, first_point_received
    if not text:
        return

    # Handle multiple JSON objects that might be in the text
    text = text.replace("} {", "}\n{").replace("}{", "}\n{")

    for line in text.split('\n'):
        line = line.strip()
        if not line:
            continue
        print(f"DEBUG: Processing line: {line}")
        sys.stdout.flush()
        start = line.find('{')
        end = line.rfind('}')
        if start == -1 or end == -1: continue
        line = line[start:end+1]

        try:
            pkt = json.loads(line)
            if "clear" in pkt:
                with state_lock:
                    data_points.clear()
                    track_chans.clear()
                    first_point_received = False
                    global_min_ms = 0.0
                    global_max_ms = 1000.0
                print("!!! Visualizer state cleared via TCP !!!")
                sys.stdout.flush()
                continue

            # Support both old and new protocol for now
            if "num_chans" in pkt:
                # New protocol
                valid = all(k in pkt for k in ["track", "ms", "chan", "val", "num_chans"])
            else:
                # Fallback/Compat
                valid = all(k in pkt for k in ["track", "channel", "ms", "val"])
                if valid:
                    pkt["chan"] = pkt["channel"]
                    pkt["num_chans"] = -1 # indicates we don't know

            if valid:
                print(f"PARSED: track={pkt['track']}, ms={pkt['ms']}, chan={pkt['chan']}, val={pkt['val']}, num_chans={pkt['num_chans']}")
                sys.stdout.flush()

                with state_lock:
                    # Update global domain
                    if not first_point_received:
                        global_min_ms = pkt['ms']
                        global_max_ms = pkt['ms']
                        first_point_received = True
                    else:
                        global_min_ms = min(global_min_ms, pkt['ms'])
                        global_max_ms = max(global_max_ms, pkt['ms'])

                    if pkt['num_chans'] > 0:
                        track_chans[pkt['track']] = max(track_chans.get(pkt['track'], 0), pkt['num_chans'])

                    if pkt['val'] == 0.0:
                        # Remove all events at this track/ms (clearing all channels)
                        data_points[:] = [p for p in data_points if not (
                            p["track"] == pkt["track"] and
                            abs(p["ms"] - pkt["ms"]) < 0.001
                        )]
                    else:
                        # Replace existing event at this track/ms if found, else append
                        found = False
                        for p in data_points:
                            if p["track"] == pkt["track"] and abs(p["ms"] - pkt["ms"]) < 0.001:
                                p["chan"] = pkt["chan"]
                                p["val"] = pkt["val"]
                                p["num_chans"] = max(p["num_chans"], pkt["num_chans"])
                                found = True
                                break
                        if not found:
                            data_points.append(pkt)
        except json.JSONDecodeError:
            continue

def tcp_server():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("", UDP_PORT))
    except Exception as e:
        print(f"ERROR: Failed to bind to port {UDP_PORT}: {e}")
        sys.exit(1)

    server_sock.listen(5)
    print(f"Visualizer: Listening on TCP port {UDP_PORT}")
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
    global global_min_ms, global_max_ms, first_point_received
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Threads~ Visualizer")
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
                        data_points.clear()
                        track_chans.clear()
                        first_point_received = False
                        global_min_ms = 0.0
                        global_max_ms = 1000.0

        screen.fill(BACKGROUND)

        with state_lock:
            points = list(data_points)
            t_chans = dict(track_chans)
            d_min = global_min_ms
            d_max = global_max_ms

        # Status text
        status_text = status_font.render(f"Events: {len(points)}  [Press 'C' to clear]", True, (150, 150, 150))
        screen.blit(status_text, (WINDOW_SIZE[0] - status_text.get_width() - 20, 20))

        # Rows: All tracks seen, and all channels for those tracks
        tracks = sorted(list(t_chans.keys()))
        all_rows = []
        for t in tracks:
            num_c = t_chans[t]
            for c in range(num_c):
                all_rows.append((t, c))

        num_rows = len(all_rows)

        span_ms = d_max - d_min
        if span_ms <= 0:
            span_ms = 1000.0

        display_min_ms = d_min - span_ms * 0.05
        display_max_ms = d_max + span_ms * 0.05
        display_span_ms = display_max_ms - display_min_ms

        left_pad = 120
        right_pad = 50
        top_pad = 80
        bottom_pad = 80

        graph_w = WINDOW_SIZE[0] - left_pad - right_pad
        graph_h = WINDOW_SIZE[1] - top_pad - bottom_pad

        row_h = graph_h / max(1, num_rows)

        # Draw rows
        for i, (t, c) in enumerate(all_rows):
            y = top_pad + i * row_h
            if i % 2 == 0:
                pygame.draw.rect(screen, (35, 35, 42), (left_pad, y, graph_w, row_h))
            pygame.draw.line(screen, (60, 60, 70), (left_pad, y), (left_pad + graph_w, y), 1)
            label = label_font.render(f"Track {t} Ch {c}", True, (200, 200, 200))
            screen.blit(label, (10, y + row_h/2 - 7))

        # Draw time axis at the bottom
        axis_y = top_pad + graph_h
        pygame.draw.line(screen, (200, 200, 200), (left_pad, axis_y), (left_pad + graph_w, axis_y), 2)
        num_ticks = 10
        for i in range(num_ticks + 1):
            tick_ms = display_min_ms + (i / num_ticks) * display_span_ms
            tick_x = left_pad + (i / num_ticks) * graph_w
            pygame.draw.line(screen, (200, 200, 200), (int(tick_x), axis_y), (int(tick_x), axis_y + 5), 2)
            tick_label = font.render(f"{tick_ms:.0f} ms", True, (200, 200, 200))
            screen.blit(tick_label, (int(tick_x) - tick_label.get_width()/2, axis_y + 10))

        # Draw points
        for p in points:
            t = p["track"]
            ms = p["ms"]
            target_c = p["chan"]
            val = p["val"]
            num_c = t_chans.get(t, 0)

            x = left_pad + (ms - display_min_ms) / display_span_ms * graph_w

            # For each channel of this track, determine what to draw
            for c in range(num_c):
                try:
                    row_idx = all_rows.index((t, c))
                except ValueError:
                    continue

                y_center = top_pad + row_idx * row_h + row_h / 2

                # Protocol:
                # If val != 0: target channel gets val, others get -999999
                # If target_c < 0: we treat all channels as receiving the val (reach message or special map)

                if target_c < 0:
                    disp_val = val
                else:
                    disp_val = val if c == target_c else -999999.0

                if disp_val == 0.0: continue # Zeros not visualized

                # Tick color
                if disp_val > 0:
                    color = (100, 255, 100) # Greenish
                    direction = 1
                else:
                    color = (255, 100, 100) # Reddish
                    direction = -1

                if disp_val == -999999.0:
                    color = (100, 100, 150) # Muted blue/gray

                tick_len = row_h * 0.4
                pygame.draw.line(screen, color, (int(x), int(y_center)), (int(x), int(y_center - direction * tick_len)), 2)

                # Label target channel with MS
                if (c == target_c or target_c == -1) and disp_val != -999999.0:
                    ms_text = f"{ms:.0f}"
                    ms_label = font.render(ms_text, True, (180, 180, 180))
                    label_y = y_center - direction * tick_len - (12 if direction > 0 else -2)
                    screen.blit(ms_label, (int(x) + 2, int(label_y)))

        pygame.display.flip()
        clock.tick(FPS)

    pygame.quit()

if __name__ == "__main__":
    listener_thread = threading.Thread(target=tcp_server, daemon=True)
    listener_thread.start()
    run_gui()

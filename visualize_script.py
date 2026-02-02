import pygame
import threading
import socket
import json
import time
import math

# Configuration
UDP_PORT = 9999
WINDOW_SIZE = (1200, 800)
BACKGROUND = (30, 30, 35)
FPS = 60

# State
data_points = [] # list of {"track": T, "channel": C, "ms": M, "val": V}
state_lock = threading.Lock()

def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Set timeout to allow thread to check for exit
    sock.settimeout(1.0)
    try:
        sock.bind(("", UDP_PORT))
    except Exception as e:
        print(f"Failed to bind to port {UDP_PORT}: {e}")
        return

    print("Visualizer: Listening on port", UDP_PORT)
    while True:
        try:
            data, addr = sock.recvfrom(65536)
            try:
                text = data.decode("utf-8", errors="replace").strip()
            except Exception as e:
                print(f"Decode error from {addr}: {e}. Hex: {data.hex()}")
                continue

            if not text:
                continue

            # Handle potential multiple JSON objects in one packet or trailing commas
            if text.endswith(','):
                text = text[:-1]

            # Simple check if it looks like JSON
            if not text.startswith('{') and not text.startswith('['):
                # Probably not JSON, ignore or log
                continue

            try:
                # Try to handle multiple objects in one packet if they are not properly separated
                # by newlines but are like {"a":1}{"b":2}
                if "}{" in text:
                    text = text.replace("}{", "}\n{")

                lines = text.split('\n')
                for line in lines:
                    line = line.strip()
                    if not line: continue
                    try:
                        pkt = json.loads(line)
                        if not isinstance(pkt, dict): continue

                        # Validate expected keys to filter out other visualizers' data
                        if all(k in pkt for k in ["track", "channel", "ms", "val"]):
                            with state_lock:
                                if pkt.get("val") == 0.0:
                                    # Remove existing points at this track, channel, and ms
                                    data_points[:] = [p for p in data_points if not (p["track"] == pkt["track"] and p["channel"] == pkt["channel"] and p["ms"] == pkt["ms"])]
                                else:
                                    data_points.append(pkt)
                    except json.JSONDecodeError:
                        continue

            except Exception as e:
                print(f"Parse error from {addr}: {e}. Data: {text[:100]}...")
        except socket.timeout:
            continue
        except Exception as e:
            print("UDP Listener error:", e)

def run_gui():
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Threads~ Visualizer")
    clock = pygame.time.Clock()
    font = pygame.font.SysFont("Arial", 10)
    label_font = pygame.font.SysFont("Arial", 14)

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        screen.fill(BACKGROUND)

        with state_lock:
            points = list(data_points)

        if points:
            # Determine tracks and channels
            tracks = sorted(list(set(p["track"] for p in points)))
            tc_map = {} # track -> set of channels
            for p in points:
                t = p["track"]
                c = p["channel"]
                if t not in tc_map: tc_map[t] = set()
                tc_map[t].add(c)

            all_rows = [] # list of (track, channel)
            for t in tracks:
                for c in sorted(list(tc_map[t])):
                    all_rows.append((t, c))

            num_rows = len(all_rows)
            min_ms = min(p["ms"] for p in points)
            max_ms = max(p["ms"] for p in points)
            # Add a small buffer to the domain
            span_ms = max_ms - min_ms
            if span_ms <= 0:
                span_ms = 1000.0

            display_min_ms = min_ms - span_ms * 0.05
            display_max_ms = max_ms + span_ms * 0.05
            display_span_ms = display_max_ms - display_min_ms

            # Padding
            left_pad = 120
            right_pad = 50
            top_pad = 50
            bottom_pad = 80

            graph_w = WINDOW_SIZE[0] - left_pad - right_pad
            graph_h = WINDOW_SIZE[1] - top_pad - bottom_pad

            row_h = graph_h / max(1, num_rows)

            # Draw rows
            for i, (t, c) in enumerate(all_rows):
                y = top_pad + i * row_h
                # Alternating row background
                if i % 2 == 0:
                    pygame.draw.rect(screen, (35, 35, 42), (left_pad, y, graph_w, row_h))

                pygame.draw.line(screen, (60, 60, 70), (left_pad, y), (left_pad + graph_w, y), 1)
                label = label_font.render(f"Track {t} Ch {c}", True, (200, 200, 200))
                screen.blit(label, (10, y + row_h/2 - 7))

            # Draw points
            for p in points:
                t = p["track"]
                c = p["channel"]
                ms = p["ms"]
                val = p["val"]

                try:
                    row_idx = all_rows.index((t, c))
                except ValueError:
                    continue

                x = left_pad + (ms - display_min_ms) / display_span_ms * graph_w
                y_center = top_pad + row_idx * row_h + row_h / 2

                # Tick color
                # Positive: Greenish, Negative: Reddish
                if val > 0:
                    color = (100, 255, 100)
                    dir = 1 # up
                else:
                    color = (255, 100, 100)
                    dir = -1 # down

                # Draw the tick
                pygame.draw.line(screen, color, (int(x), int(y_center)), (int(x), int(y_center - dir * row_h * 0.4)), 2)

                # Label with MS
                ms_text = f"{ms:.0f}"
                ms_label = font.render(ms_text, True, (180, 180, 180))
                label_y = y_center - dir * row_h * 0.4 - (12 if dir > 0 else -2)
                screen.blit(ms_label, (int(x) + 2, int(label_y)))

            # Draw time axis at the bottom
            axis_y = top_pad + graph_h
            pygame.draw.line(screen, (200, 200, 200), (left_pad, axis_y), (left_pad + graph_w, axis_y), 2)

            # Time labels
            num_ticks = 10
            for i in range(num_ticks + 1):
                tick_ms = display_min_ms + (i / num_ticks) * display_span_ms
                tick_x = left_pad + (i / num_ticks) * graph_w
                pygame.draw.line(screen, (200, 200, 200), (int(tick_x), axis_y), (int(tick_x), axis_y + 5), 2)
                tick_label = font.render(f"{tick_ms:.0f} ms", True, (200, 200, 200))
                screen.blit(tick_label, (int(tick_x) - tick_label.get_width()/2, axis_y + 10))

        pygame.display.flip()
        clock.tick(FPS)

    pygame.quit()

if __name__ == "__main__":
    t = threading.Thread(target=udp_listener, daemon=True)
    t.start()
    run_gui()

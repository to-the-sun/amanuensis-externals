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
data_points = [] # list of {"track": T, "channel": C, "ms": M, "val": V}
state_lock = threading.Lock()

def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Allow address reuse
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # Set timeout to allow thread to check for exit
    sock.settimeout(1.0)
    try:
        sock.bind(("", UDP_PORT))
    except Exception as e:
        print(f"ERROR: Failed to bind to port {UDP_PORT}: {e}")
        print("Maybe another visualizer is running?")
        sys.exit(1)

    print(f"Visualizer: Listening on UDP port {UDP_PORT}")
    sys.stdout.flush()

    while True:
        try:
            data, addr = sock.recvfrom(65536)
            try:
                text = data.decode("utf-8", errors="replace").strip()
                # Print all received data for debugging
                print(f"UDP REC: {text}")
                sys.stdout.flush()
            except Exception as e:
                print(f"DECODE ERROR: {e}")
                sys.stdout.flush()
                continue

            if not text:
                continue

            # Robust parsing for potentially multiple or concatenated JSON objects
            text = text.replace("} {", "}\n{")
            text = text.replace("}{", "}\n{")

            lines = text.split('\n')
            for line in lines:
                line = line.strip()
                if not line: continue
                # Handle leading/trailing junk
                start = line.find('{')
                end = line.rfind('}')
                if start == -1 or end == -1: continue
                line = line[start:end+1]

                try:
                    pkt = json.loads(line)
                    if not isinstance(pkt, dict): continue

                    if all(k in pkt for k in ["track", "channel", "ms", "val"]):
                        print(f"PARSED: track={pkt['track']}, ch={pkt['channel']}, ms={pkt['ms']}, val={pkt['val']}")
                        sys.stdout.flush()
                        with state_lock:
                            if pkt.get("val") == 0.0:
                                # Remove existing points at this track, channel, and ms
                                data_points[:] = [p for p in data_points if not (
                                    p["track"] == pkt["track"] and
                                    p["channel"] == pkt["channel"] and
                                    abs(p["ms"] - pkt["ms"]) < 0.001
                                )]
                            else:
                                # Update existing point if it matches track/chan/ms, else append
                                found = False
                                for p in data_points:
                                    if p["track"] == pkt["track"] and p["channel"] == pkt["channel"] and abs(p["ms"] - pkt["ms"]) < 0.001:
                                        p["val"] = pkt["val"]
                                        found = True
                                        break
                                if not found:
                                    data_points.append(pkt)
                    else:
                        # Ignore messages meant for other visualizers but maybe log them once in a while?
                        # For now, just skip silently if it doesn't match our schema.
                        pass
                except json.JSONDecodeError:
                    continue

        except socket.timeout:
            continue
        except Exception as e:
            print("UDP Listener error:", e)
            sys.stdout.flush()

def run_gui():
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

        screen.fill(BACKGROUND)

        with state_lock:
            points = list(data_points)

        # Status text
        status_text = status_font.render(f"Points: {len(points)}  [Press 'C' to clear]", True, (150, 150, 150))
        screen.blit(status_text, (WINDOW_SIZE[0] - status_text.get_width() - 20, 20))

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

            span_ms = max_ms - min_ms
            if span_ms <= 0:
                span_ms = 1000.0

            display_min_ms = min_ms - span_ms * 0.05
            display_max_ms = max_ms + span_ms * 0.05
            display_span_ms = display_max_ms - display_min_ms

            # Padding
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
                if val > 0:
                    color = (100, 255, 100) # Greenish
                    direction = 1 # up
                else:
                    color = (255, 100, 100) # Reddish
                    direction = -1 # down

                # Special value -999999.0
                if val == -999999.0:
                    color = (100, 100, 150) # Muted blue/gray

                # Draw the tick
                tick_len = row_h * 0.4
                pygame.draw.line(screen, color, (int(x), int(y_center)), (int(x), int(y_center - direction * tick_len)), 2)

                # Label with MS
                if val != -999999.0:
                    ms_text = f"{ms:.0f}"
                    ms_label = font.render(ms_text, True, (180, 180, 180))
                    label_y = y_center - direction * tick_len - (12 if direction > 0 else -2)
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
    listener_thread = threading.Thread(target=udp_listener, daemon=True)
    listener_thread.start()
    run_gui()

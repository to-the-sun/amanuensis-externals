import pygame
import threading
import socket
import json
import time
import os
import sys
import traceback

# Set dummy video driver for headless environments
if os.environ.get('HEADLESS'):
    os.environ['SDL_VIDEODRIVER'] = 'dummy'
    os.environ['SDL_AUDIODRIVER'] = 'dummy'

# Configuration
TCP_PORT = 9999
WINDOW_SIZE = (1200, 800)
FPS = 60

# State
state = {
    "tracks": {},
    "song_reach": 0,
    "bar_length": 125,
    "events": []
}
state_lock = threading.Lock()

def process_packet(text):
    if not text:
        return
    text = text.replace("} {", "}\n{").replace("}{", "}\n{")
    for line in text.split('\n'):
        line = line.strip()
        if not line: continue
        try:
            pkt = json.loads(line)
            if pkt.get("type") != "crucible":
                continue

            with state_lock:
                state["song_reach"] = pkt.get("song_reach", state.get("song_reach", 0))
                state["bar_length"] = pkt.get("bar_length", state.get("bar_length", 125))
                if "tracks" in pkt:
                    state["tracks"] = pkt["tracks"]

                if pkt.get("event") == "new_span":
                    track = pkt.get("new_span_track")
                    bars = pkt.get("new_span_bars", [])
                    rating = pkt.get("new_span_rating", 0.0)
                    state["events"].append({
                        "type": "new_span",
                        "track": track,
                        "bars": bars,
                        "rating": rating,
                        "start_time": time.time(),
                        "duration": 3.0
                    })
        except json.JSONDecodeError:
            continue

def tcp_server():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("", TCP_PORT))
    except Exception as e:
        print(f"ERROR: Failed to bind to port {TCP_PORT}: {e}")
        return
    server_sock.listen(5)
    print(f"Crucible Visualizer: Listening on port {TCP_PORT}")
    while True:
        try:
            client_sock, _ = server_sock.accept()
            threading.Thread(target=handle_client, args=(client_sock,), daemon=True).start()
        except: break

def handle_client(sock):
    buffer = ""
    while True:
        try:
            data = sock.recv(4096)
            if not data: break
            buffer += data.decode("utf-8", errors="replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                process_packet(line.strip())
        except: break
    sock.close()

def run_gui():
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Crucible Visualizer")
    clock = pygame.time.Clock()
    font = pygame.font.SysFont("Arial", 12)
    big_font = pygame.font.SysFont("Arial", 20)

    while True:
        now = time.time()
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                pygame.quit(); sys.exit()

        with state_lock:
            tracks = state["tracks"].copy()
            song_reach = state["song_reach"]
            bar_length = state["bar_length"]
            events = state["events"][:]
            # Clean up old events
            state["events"] = [e for e in events if now - e["start_time"] < e["duration"]]

        screen.fill((30, 30, 35))

        if not tracks and song_reach == 0:
            pygame.display.flip()
            clock.tick(FPS)
            continue

        margin_left = 60
        margin_top = 40
        cell_w = 20
        cell_h = 20

        # Determine track rows
        sorted_track_ids = sorted(tracks.keys(), key=lambda x: int(x) if x.isdigit() else 999)
        track_to_row = {tid: i for i, tid in enumerate(sorted_track_ids)}

        num_cols = (song_reach // bar_length) + 1 if bar_length > 0 else 0

        # Draw background grid lines
        for i in range(len(sorted_track_ids) + 1):
            y = margin_top + i * cell_h
            pygame.draw.line(screen, (60, 60, 65), (margin_left, y), (margin_left + num_cols * cell_w, y))
        for j in range(int(num_cols) + 1):
            x = margin_left + j * cell_w
            pygame.draw.line(screen, (60, 60, 65), (x, margin_top), (x, margin_top + len(sorted_track_ids) * cell_h))

        # Draw track labels
        for tid, row in track_to_row.items():
            lbl = font.render(f"T {tid}", True, (180, 180, 180))
            screen.blit(lbl, (10, margin_top + row * cell_h + (cell_h - lbl.get_height())//2))

        # Draw filled boxes for present bars
        for tid, bars in tracks.items():
            if tid not in track_to_row: continue
            row = track_to_row[tid]
            for bar_ts in bars:
                if bar_length <= 0: continue
                col = bar_ts // bar_length
                rect = pygame.Rect(margin_left + col * cell_w + 1, margin_top + row * cell_h + 1, cell_w - 1, cell_h - 1)
                pygame.draw.rect(screen, (80, 80, 100), rect)

        # Draw event animations
        for e in events:
            elapsed = now - e["start_time"]
            t = elapsed / e["duration"]
            if t > 1.0: continue

            tid = e["track"]
            if tid not in track_to_row: continue
            row = track_to_row[tid]

            # Flashing/Bright Boxes
            # Flash for 0.5s then stay bright
            is_flashing = elapsed < 0.5 and (int(elapsed * 10) % 2 == 0)
            color = (255, 255, 255) if is_flashing else (150, 150, 220)

            for bar_ts in e["bars"]:
                if bar_length <= 0: continue
                col = bar_ts // bar_length
                rect = pygame.Rect(margin_left + col * cell_w + 1, margin_top + row * cell_h + 1, cell_w - 1, cell_h - 1)
                pygame.draw.rect(screen, color, rect)

            # Floating Rating
            if e["bars"] and bar_length > 0:
                avg_col = sum(b // bar_length for b in e["bars"]) / len(e["bars"])
                float_x = margin_left + avg_col * cell_w
                float_y = margin_top + row * cell_h - (elapsed * 50) # Rise 50px/s

                alpha = int(255 * (1.0 - t))
                rating_text = big_font.render(f"{e['rating']:.2f}", True, (255, 255, 100))
                rating_text.set_alpha(alpha)
                screen.blit(rating_text, (float_x, float_y))

        pygame.display.flip()
        clock.tick(FPS)

if __name__ == "__main__":
    threading.Thread(target=tcp_server, daemon=True).start()
    try:
        run_gui()
    except Exception:
        traceback.print_exc()
        print("\nVisualizer encountered an error and has stopped.")
        print("Press Enter to exit...")
        try:
            input()
        except EOFError:
            # Handle non-interactive environments
            time.sleep(3600)

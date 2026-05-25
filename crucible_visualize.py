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
FPS = 60

# State
state = {
    "tracks": {},
    "song_reach": 0,
    "bar_length": 125,
    "events": []
}
state_lock = threading.Lock()

def recalculate_reach():
    """Recalculates state['song_reach'] based on the furthest bar in state['tracks']."""
    max_reach = 0
    bar_length = state.get("bar_length", 125)
    for track_bars in state["tracks"].values():
        for bar_ts in track_bars:
            try:
                b_ts = float(bar_ts)
                if b_ts + bar_length > max_reach:
                    max_reach = b_ts + bar_length
            except (ValueError, TypeError):
                continue
    state["song_reach"] = max_reach

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
                state["bar_length"] = pkt.get("bar_length", state.get("bar_length", 125))

                dirty = False
                if "tracks" in pkt:
                    state["tracks"] = pkt["tracks"]
                    dirty = True

                if pkt.get("event") == "new_span":
                    track = pkt.get("new_span_track")
                    bars = pkt.get("new_span_bars", [])
                    rating = pkt.get("new_span_rating", 0.0)

                    # Update local track state from the new span event
                    if track is not None:
                        t_str = str(track)
                        if t_str not in state["tracks"]:
                            state["tracks"][t_str] = []

                        existing_bars = set(state["tracks"][t_str])
                        added = False
                        for b in bars:
                            if b not in existing_bars:
                                state["tracks"][t_str].append(b)
                                added = True
                        if added:
                            dirty = True

                    state["events"].append({
                        "type": "new_span",
                        "track": track,
                        "bars": bars,
                        "rating": rating,
                        "start_time": time.time(),
                        "duration": 3.0
                    })

                if dirty:
                    recalculate_reach()

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
    info = pygame.display.Info()
    # Fallback to 1920x1080 if info doesn't provide valid dimensions
    screen_w = info.current_w if info.current_w > 0 else 1920
    screen_h = info.current_h if info.current_h > 0 else 1080

    last_num_tracks = -1
    screen = None
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

        # Determine track rows
        sorted_track_ids = sorted(tracks.keys(), key=lambda x: int(x) if x.isdigit() else 999)
        num_tracks = len(sorted_track_ids)

        margin_left = 100
        margin_top = 40
        margin_bottom = 20
        margin_right = 40
        cell_h = 20
        display_tracks = max(4, num_tracks)

        if display_tracks != last_num_tracks:
            target_height = margin_top + display_tracks * cell_h + margin_bottom
            os.environ['SDL_VIDEO_WINDOW_POS'] = "0,%d" % (screen_h - target_height - 40)
            screen = pygame.display.set_mode((screen_w, int(target_height)))
            last_num_tracks = display_tracks

        screen.fill((30, 30, 35))

        if not tracks and song_reach == 0:
            pygame.display.flip()
            clock.tick(FPS)
            continue

        track_to_row = {tid: i for i, tid in enumerate(sorted_track_ids)}

        num_cols = (song_reach // bar_length) + 1 if bar_length > 0 else 0
        cell_w = (screen_w - margin_left - margin_right) / max(1, num_cols)

        # Draw background grid lines
        for i in range(len(sorted_track_ids) + 1):
            y = margin_top + i * cell_h
            pygame.draw.line(screen, (60, 60, 65), (margin_left, y), (margin_left + num_cols * cell_w, y))
        for j in range(int(num_cols) + 1):
            x = margin_left + j * cell_w
            pygame.draw.line(screen, (60, 60, 65), (x, margin_top), (x, margin_top + len(sorted_track_ids) * cell_h))

        # Draw time legend
        sample_lbl = font.render("00:00", True, (0,0,0))
        label_w = sample_lbl.get_width()
        step = max(1, int((label_w * 2) // cell_w) + 1) if cell_w > 0 else 1
        for j in range(0, int(num_cols) + 1, step):
            x = margin_left + j * cell_w
            total_seconds = (j * bar_length) // 1000
            time_str = f"{int(total_seconds // 60)}:{int(total_seconds % 60):02d}"
            lbl = font.render(time_str, True, (180, 180, 180))
            screen.blit(lbl, (x - lbl.get_width() // 2, margin_top - lbl.get_height() - 5))

        # Draw track labels
        for tid, row in track_to_row.items():
            lbl = font.render(f"T {tid}", True, (180, 180, 180))
            screen.blit(lbl, (margin_left - lbl.get_width() - 10, margin_top + row * cell_h + (cell_h - lbl.get_height())//2))

        # Draw filled boxes for present bars
        for tid, bars in tracks.items():
            if tid not in track_to_row: continue
            row = track_to_row[tid]
            for bar_ts in bars:
                try:
                    b_ts = int(bar_ts)
                except (ValueError, TypeError): continue
                if bar_length <= 0: continue
                col = b_ts // bar_length
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

            valid_bars = []
            for bar_ts in e["bars"]:
                try:
                    b_ts = int(bar_ts)
                    valid_bars.append(b_ts)
                except (ValueError, TypeError): continue
                if bar_length <= 0: continue
                col = b_ts // bar_length
                rect = pygame.Rect(margin_left + col * cell_w + 1, margin_top + row * cell_h + 1, cell_w - 1, cell_h - 1)
                pygame.draw.rect(screen, color, rect)

            # Floating Rating
            if valid_bars and bar_length > 0:
                avg_col = sum(b // bar_length for b in valid_bars) / len(valid_bars)
                float_x = margin_left + avg_col * cell_w + (cell_w / 2)
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

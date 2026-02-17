import pygame
import threading
import socket
import json
import time
import math
import os

# Set dummy video driver for headless environments
if os.environ.get('HEADLESS'):
    os.environ['SDL_VIDEODRIVER'] = 'dummy'
    os.environ['SDL_AUDIODRIVER'] = 'dummy'

# Configuration
TCP_PORT = 9999
WINDOW_SIZE = (1000, 600)
BACKGROUND = (38, 38, 46)  # close to sketch.glclearcolor(0.15...)
FPS = 60

# State (shared between threads; guarded by lock)
state = {
    "palettes": {},
    "bar_length": 125.0
}
state_lock = threading.Lock()

def process_pkt(text):
    if not text:
        return
    try:
        pkt = json.loads(text)
        with state_lock:
            if "palettes" in pkt:
                state["palettes"] = pkt["palettes"]

            if "bar_length" in pkt:
                state["bar_length"] = float(pkt["bar_length"])

            if "building" in pkt and "palettes" not in pkt:
                # Compatibility for single-palette packets
                state["palettes"] = {
                    "default": {
                        "building": pkt["building"],
                        "current_offset": pkt.get("current_offset", 0.0)
                    }
                }
    except json.JSONDecodeError as e:
        print(f"JSON parse error: {e}")

def tcp_server():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("", TCP_PORT))
    except Exception as e:
        print(f"ERROR: Failed to bind to port {TCP_PORT}: {e}")
        return

    server_sock.listen(5)
    print("GUI: Listening for TCP on port", TCP_PORT)
    while True:
        try:
            client_sock, addr = server_sock.accept()
            print(f"Accepted connection from {addr}")
            threading.Thread(target=handle_client, args=(client_sock,), daemon=True).start()
        except Exception as e:
            print(f"TCP Server error: {e}")

def handle_client(sock):
    buffer = ""
    while True:
        try:
            data = sock.recv(4096)
            if not data:
                break
            text = data.decode("utf-8", errors="ignore")
            buffer += text
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                process_pkt(line.strip())
        except Exception as e:
            print(f"Client handler error: {e}")
            break
    sock.close()


error_message = None  # global error flag/message

# Main draw routine using pygame
def run_gui():
    global error_message
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Building Spans Visualizer")
    clock = pygame.time.Clock()
    font = pygame.font.SysFont("Arial", 16)
    large_font = pygame.font.SysFont("Arial", 20)
    small_font = pygame.font.SysFont("Arial", 12)

    running = True
    while running:
        try:
            if error_message:
                # Show error message, don't update grid
                screen.fill((60, 0, 0))
                txt = large_font.render("ERROR: " + error_message, True, (255, 200, 200))
                rect = txt.get_rect(center=(WINDOW_SIZE[0] // 2, WINDOW_SIZE[1] // 2))
                screen.blit(txt, rect)
                pygame.display.flip()
                for event in pygame.event.get():
                    if event.type == pygame.QUIT:
                        running = False
                clock.tick(FPS)
                continue

            now = time.time()
            for event in pygame.event.get():
                if event.type == pygame.QUIT:
                    running = False

            # Take a thread-safe snapshot of state to use in rendering
            with state_lock:
                palettes = state.get("palettes", {})
                bar_length = state.get("bar_length", 125.0)

            # background
            screen.fill(BACKGROUND)

            if not palettes:
                pygame.display.flip()
                clock.tick(FPS)
                continue

            w, h = WINDOW_SIZE
            num_palettes = len(palettes)
            palette_h = (h - 60) / num_palettes # margin for labels

            grid_left = 60
            grid_right = w - 40
            grid_w = grid_right - grid_left

            sorted_palettes = sorted(palettes.keys())

            for p_idx, p_name in enumerate(sorted_palettes):
                p_data = palettes[p_name]
                working_memory = p_data.get("building", {})
                current_offset = p_data.get("current_offset", 0.0)

                p_top = 40 + p_idx * palette_h
                p_timeline_h = palette_h * 0.8

                # Palette Label
                p_label = large_font.render(f"Palette: {p_name}", True, (255, 255, 255))
                screen.blit(p_label, (grid_left, p_top - 25))

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
                    pygame.draw.rect(screen, (20, 20, 25), timeline_rect)

                    # Draw min and max labels for the timeline
                    min_label = small_font.render(f"{min_ts:.2f}", True, (204, 204, 204))
                    max_label = small_font.render(f"{max_ts:.2f}", True, (204, 204, 204))
                    screen.blit(min_label, (grid_left, p_top + p_timeline_h + 5))
                    screen.blit(max_label, (grid_right - max_label.get_width(), p_top + p_timeline_h + 5))

                    if working_memory:
                        # Each unique track-offset identifier gets its own row
                        sorted_track_keys = sorted(working_memory.keys(), key=lambda k: int(k.split('-')[0]))
                        track_h = p_timeline_h / max(1, len(sorted_track_keys))

                        for i, track_id in enumerate(sorted_track_keys):
                            track_data = working_memory[track_id]
                            track_y = p_top + i * track_h

                            # Draw track label
                            track_label = font.render(f"T {track_id}", True, (204, 204, 204))
                            screen.blit(track_label, (5, track_y + track_h / 2 - track_label.get_height() / 2))

                            # Draw horizontal bars for spans (drawn first, in the background)
                            span_data = track_data.get("span", [])
                            if span_data:
                                try:
                                    offset_val = float(track_id.split('-')[1])

                                    # The main container bar. The timestamps in span_data are relative.
                                    # To get the absolute position, we need to add the track's offset.
                                    min_abs_span_ts = min(span_data) + offset_val
                                    max_abs_span_ts = max(span_data) + offset_val + bar_length # Add bar_length to get the end of the last bar

                                    start_x = grid_left + grid_w * (min_abs_span_ts - min_ts) / span_ts
                                    end_x = grid_left + grid_w * (max_abs_span_ts - min_ts) / span_ts

                                    bar_y = track_y + track_h * 0.5 # Center the bar vertically
                                    bar_height = track_h * 0.4

                                    # Use a surface to handle opacity
                                    s = pygame.Surface((max(1, end_x - start_x), bar_height), pygame.SRCALPHA)
                                    s.fill((60, 60, 100, 128)) # Dull blue with alpha
                                    screen.blit(s, (start_x, bar_y - bar_height / 2))


                                    # Draw individual bars within the span and their relative labels
                                    for bar_relative_ts in span_data:
                                        # Calculate absolute position for drawing
                                        bar_abs_start_ts = bar_relative_ts + offset_val
                                        bar_start_x = grid_left + grid_w * (bar_abs_start_ts - min_ts) / span_ts

                                        # Calculate width in pixels based on bar_length
                                        bar_width_pixels = (grid_w * bar_length) / span_ts

                                        s = pygame.Surface((max(1, bar_width_pixels), bar_height), pygame.SRCALPHA)
                                        s.fill((90, 90, 130, 128)) # Slightly lighter dull blue
                                        screen.blit(s, (bar_start_x, bar_y - bar_height / 2))

                                        # Label with the relative timestamp from the span data
                                        label_text = f"{bar_relative_ts:.0f}"
                                        label = small_font.render(label_text, True, (204, 204, 204))
                                        screen.blit(label, (bar_start_x + 2, bar_y - bar_height / 2 - 15))

                                except (ValueError, IndexError):
                                    # Handle cases where the track_id format is unexpected
                                    pass

                            # Draw hash marks for absolutes
                            for ts in track_data.get("absolutes", []):
                                x = grid_left + grid_w * (ts - min_ts) / span_ts
                                pygame.draw.line(screen, (100, 200, 100), (x, track_y), (x, track_y + track_h), 1)
                                label = small_font.render(f"{ts:.2f}", True, (100, 200, 100))
                                screen.blit(label, (x + 2, track_y + 5))

                        # Collect all unique offsets from all tracks in this palette
                        palette_offsets = set()
                        for track_data in working_memory.values():
                            palette_offsets.update(track_data.get("offsets", []))

                        # Draw all offset hash marks on all tracks in this palette
                        for i, track_id in enumerate(sorted_track_keys):
                            track_y = p_top + i * track_h
                            for ts in palette_offsets:
                                x = grid_left + grid_w * (ts - min_ts) / span_ts
                                pygame.draw.line(screen, (200, 100, 100), (x, track_y), (x, track_y + track_h), 2)
                                # Label offsets only on the top track of the palette to avoid clutter
                                if i == 0:
                                    label = small_font.render(f"{ts:.0f}", True, (200, 100, 100))
                                    screen.blit(label, (x + 2, p_top - 15))

            pygame.display.flip()
            clock.tick(FPS)
        except Exception as e:
            error_message = str(e)
            print("GUI: Error in main loop:", error_message)

    pygame.quit()

if __name__ == "__main__":
    # start TCP server thread
    t = threading.Thread(target=tcp_server, daemon=True)
    t.start()
    run_gui()

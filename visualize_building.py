import pygame
import threading
import socket
import json
import time
import math

# Configuration
UDP_PORT = 9999
WINDOW_SIZE = (1000, 800)
BACKGROUND = (38, 38, 46)  # close to sketch.glclearcolor(0.15...)
FPS = 60
FLASH_DURATION = 0.5  # seconds (500 ms)
X_FLASH_DURATION = 0.35

# State (shared between threads; guarded by lock)
state = {
    "song_length": 0.0,
    "measure_length": 1.0,
    "transcript": {},
    "working_memory": {},
    "current_offset": 0.0
}
state_lock = threading.Lock()

# Flash state per cell: key "track_measure" -> { flash_until, flash_color, x_until }
flash_state = {}

# UDP listener: receives JSON packets and merges into `state`
def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", UDP_PORT))
    print("GUI: Listening for UDP on port", UDP_PORT)
    while True:
        try:
            data, addr = sock.recvfrom(65536)
            text = data.decode("utf-8", errors="ignore").rstrip(",").strip()
            print("UDP recv from {}: {}".format(addr, text))
            if not text:
                continue

            # The C code sends a single JSON object per UDP packet.
            # It might have a trailing comma from a previous bug, so we strip it.
            if text.endswith(','):
                text = text[:-1]

            try:
                pkt = json.loads(text)
            except json.JSONDecodeError as e:
                print(f"UDP listener: JSON parse error: {e}")
                continue

            with state_lock:
                if "building" in pkt:
                    state["working_memory"] = pkt["building"]
                if "current_offset" in pkt:
                    state["current_offset"] = float(pkt["current_offset"])

        except Exception as e:
            print(f"UDP listener: Unexpected error: {e}")

# Helper to prune flash_state when transcript replacement/deletion occurs.
def _prune_flash_state_unlocked():
    # called with state_lock held
    valid_keys = set()
    for tKey, measures in state["transcript"].items():
        for mKey in measures.keys():
            valid_keys.add(f"{tKey}_{mKey}")
    for k in list(flash_state.keys()):
        if k not in valid_keys:
            del flash_state[k]

# Helper to compute span groups per track: returns { trackKey: { spanValue: [cols...] } }
def compute_span_groups(transcript, measure_length, numColumns):
    groups = {}
    for rowTrack in transcript.keys():
        groups[rowTrack] = {}
        measures = transcript[rowTrack]
        for col in range(numColumns):
            measureStart = str(int(round(col * measure_length)))
            item = measures.get(measureStart)
            if item and "span" in item:
                sp = item["span"]
                groups[rowTrack].setdefault(str(sp), []).append(col)
    return groups

error_message = None  # global error flag/message

# Main draw routine using pygame
def run_gui():
    global error_message
    pygame.init()
    screen = pygame.display.set_mode(WINDOW_SIZE)
    pygame.display.set_caption("Transcript GUI (UDP port {})".format(UDP_PORT))
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
                song_length = float(state.get("song_length", 0) or 0)
                measure_length = float(state.get("measure_length", 1) or 1)
                transcript = {tk: {mk: dict(v) for mk, v in ms.items()} for tk, ms in state.get("transcript", {}).items()}
                flash_snapshot = {k: dict(v) for k, v in flash_state.items()}
                working_memory = state.get("working_memory", {})
                current_offset = state.get("current_offset", 0.0)

            # compute layout
            numTracks = max(1, 4)
            numColumns = int(math.ceil(song_length / measure_length)) if measure_length > 0 else 1
            if numColumns < 1:
                numColumns = 1

            w, h = WINDOW_SIZE
            aspect = w / float(h)

            timeline_top = 40
            timeline_h = 200

            grid_left = 40
            grid_right = w - 40
            grid_top = timeline_top + timeline_h + 40
            grid_bottom = h - 40
            grid_w = grid_right - grid_left
            grid_h = grid_bottom - grid_top

            cellWidth = grid_w / max(1, numColumns)
            cellHeight = grid_h / max(1, numTracks)

            # background
            screen.fill(BACKGROUND)

            # draw working_memory timeline
            all_ts = [ts for track_data in working_memory.values() for ts_type in track_data.values() for ts in ts_type]
            if current_offset is not None:
                all_ts.append(current_offset)

            if all_ts:
                min_ts, max_ts = min(all_ts), max(all_ts)
                span_ts = max_ts - min_ts if max_ts > min_ts else 1.0

                timeline_rect = pygame.Rect(grid_left, timeline_top, grid_w, timeline_h)
                pygame.draw.rect(screen, (20, 20, 25), timeline_rect)

                # Draw min and max labels for the timeline
                min_label = small_font.render(f"{min_ts:.2f}", True, (204, 204, 204))
                max_label = small_font.render(f"{max_ts:.2f}", True, (204, 204, 204))
                screen.blit(min_label, (grid_left, timeline_top + timeline_h + 5))
                screen.blit(max_label, (grid_right - max_label.get_width(), timeline_top + timeline_h + 5))

                if working_memory:
                    # Each unique track-offset identifier gets its own row
                    sorted_track_keys = sorted(working_memory.keys(), key=lambda k: int(k.split('-')[0]))
                    track_h = timeline_h / max(1, len(sorted_track_keys))

                    for i, track_id in enumerate(sorted_track_keys):
                        track_data = working_memory[track_id]
                        track_y = timeline_top + i * track_h

                        # Draw track label
                        track_label = font.render(f"Track {track_id}", True, (204, 204, 204))
                        screen.blit(track_label, (5, track_y + track_h / 2 - track_label.get_height() / 2))

                        # Draw hash marks for absolutes
                        for ts in track_data.get("absolutes", []):
                            x = grid_left + grid_w * (ts - min_ts) / span_ts
                            pygame.draw.line(screen, (100, 200, 100), (x, track_y), (x, track_y + track_h), 1)
                            label = small_font.render(f"{ts:.2f}", True, (100, 200, 100))
                            screen.blit(label, (x + 2, track_y + 5))

                        # Draw hash marks for offsets
                        for ts in track_data.get("offsets", []):
                            x = grid_left + grid_w * (ts - min_ts) / span_ts
                            pygame.draw.line(screen, (200, 100, 100), (x, track_y), (x, track_y + track_h), 2)
                            label = small_font.render(f"{ts:.2f}", True, (200, 100, 100))
                            screen.blit(label, (x + 2, track_y + 20))

                        # Draw horizontal bars for spans
                        span_data = track_data.get("span", [])
                        if span_data:
                            # Extract the offset from the track_id
                            try:
                                offset = float(track_id.split('-')[1])

                                # Find the min and max timestamps *within the span* for this specific track
                                min_span_ts = min(span_data)
                                max_span_ts = max(span_data)

                                # Calculate the screen coordinates for the start and end of the span bar
                                start_x = grid_left + grid_w * (min_span_ts - min_ts) / span_ts
                                end_x = grid_left + grid_w * (max_span_ts - min_ts) / span_ts

                                # Draw the main bar for the span
                                bar_y = track_y + track_h * 0.5 # Center the bar vertically
                                bar_height = track_h * 0.4
                                pygame.draw.rect(screen, (100, 100, 255), (start_x, bar_y - bar_height / 2, end_x - start_x, bar_height))

                                # Draw individual bars within the span and their relative labels
                                for bar_ts in span_data:
                                    bar_start_x = grid_left + grid_w * (bar_ts - min_ts) / span_ts
                                    # Assuming a fixed length for now, e.g., 10 pixels wide or based on a property if available
                                    bar_width = 5
                                    pygame.draw.rect(screen, (200, 200, 255), (bar_start_x, bar_y - bar_height / 2, bar_width, bar_height))

                                    # Label with relative timestamp
                                    relative_ts = bar_ts - offset
                                    label_text = f"{relative_ts:.0f}"
                                    label = small_font.render(label_text, True, (255, 255, 255))
                                    screen.blit(label, (bar_start_x + 2, bar_y - bar_height / 2 - 15))


                            except (ValueError, IndexError):
                                # Handle cases where the track_id format is unexpected
                                pass


                    # Draw a single line for the current global offset across all tracks
                    if current_offset is not None:
                         x = grid_left + grid_w * (current_offset - min_ts) / span_ts
                         pygame.draw.line(screen, (255, 100, 100), (x, timeline_top), (x, timeline_top + timeline_h), 2)
                         label = small_font.render(f"{current_offset:.2f}", True, (255, 100, 100))
                         screen.blit(label, (x + 2, timeline_top - 15))


            # draw measure start labels
            for col in range(numColumns):
                x = grid_left + col * cellWidth + cellWidth / 2
                measureStart = str(int(round(col * measure_length)))
                txt = font.render(measureStart, True, (180, 180, 230))
                rect = txt.get_rect(center=(x, grid_top - 12))
                screen.blit(txt, rect)

            # draw grid cells
            for row in range(numTracks):
                trackKey = str(row + 1)
                for col in range(numColumns):
                    x = grid_left + col * cellWidth
                    y = grid_top + row * cellHeight
                    measureStart = str(int(round(col * measure_length)))
                    cellKey = f"{trackKey}_{measureStart}"
                    hasMeasure = trackKey in transcript and measureStart in transcript[trackKey]

                    # default color and alpha
                    base_color = (51, 128, 204)  # blue
                    alpha = 255
                    ratingToShow = None

                    fs = flash_snapshot.get(cellKey, {})
                    cell_entry = transcript.get(trackKey, {}).get(measureStart, {})

                    # Helper: extract scalar value from rating (if list, use first element)
                    def _scalar(val):
                        if isinstance(val, list) and val:
                            val = val[0]
                        if isinstance(val, str):
                            try:
                                if "." in val:
                                    return float(val)
                                else:
                                    return int(val)
                            except Exception:
                                return None
                        return val

                    # If currently flashing (triggered at UDP receipt)
                    if fs.get("flash_until", 0) > now:
                        colr = fs.get("flash_color", (255, 153, 26))
                        draw_color = colr
                        # Use rating from transcript for alpha if present
                        ratingToShow = _scalar(cell_entry.get("rating"))
                        if ratingToShow is not None:
                            try:
                                alpha = int(max(0, min(1, ratingToShow)) * 255)
                            except Exception as e:
                                print("GUI: Error computing alpha from rating:", e)
                                alpha = 255
                    else:
                        # normal display uses current transcript rating
                        ratingToShow = _scalar(cell_entry.get("rating"))
                        draw_color = base_color
                        if ratingToShow is not None:
                            try:
                                alpha = int(max(0, min(1, ratingToShow)) * 255)
                            except Exception as e:
                                print("GUI: Error computing alpha from rating:", e)
                                alpha = 255

                    # fill cell if measure exists
                    if hasMeasure:
                        surf = pygame.Surface((int(cellWidth)+1, int(cellHeight)+1), pygame.SRCALPHA)
                        surf.fill((draw_color[0], draw_color[1], draw_color[2], alpha))
                        screen.blit(surf, (int(x), int(y)))

                        # rating text
                        if ratingToShow is not None:
                            txt = large_font.render(str(ratingToShow), True, (244, 244, 244))
                            rect = txt.get_rect(center=(x + cellWidth / 2, y + cellHeight / 2))
                            screen.blit(txt, rect)

                    # cell border
                    pygame.draw.rect(screen, (77, 77, 77), (int(x), int(y), int(cellWidth), int(cellHeight)), 1)

                    # draw flashX if present and active
                    if fs.get("x_until", 0) > now:
                        cx = x + cellWidth / 2
                        cy = y + cellHeight / 2
                        size = min(cellWidth, cellHeight) * 0.7
                        pygame.draw.line(screen, (255, 0, 0), (cx - size/2, cy - size/2), (cx + size/2, cy + size/2), 3)
                        pygame.draw.line(screen, (255, 0, 0), (cx + size/2, cy - size/2), (cx - size/2, cy + size/2), 3)

            # compute and draw span group borders (per track)
            spanGroups = compute_span_groups(transcript, measure_length, numColumns)
            for trackKey, groups in spanGroups.items():
                for spanVal, cols in groups.items():
                    if not cols: continue
                    row = int(trackKey) - 1
                    minCol = min(cols)
                    maxCol = max(cols)
                    x1 = grid_left + minCol * cellWidth
                    y1 = grid_top + row * cellHeight
                    x2 = grid_left + (maxCol + 1) * cellWidth
                    y2 = grid_top + (row + 1) * cellHeight
                    pygame.draw.rect(screen, (255, 51, 51), (int(x1), int(y1), int(x2 - x1), int(y2 - y1)), 3)

            # draw track labels
            for row in range(numTracks):
                labelY = grid_top + row * cellHeight + cellHeight / 2
                txt = font.render("Track " + str(row + 1), True, (204, 204, 204))
                rect = txt.get_rect(left=4, centery=labelY)
                screen.blit(txt, rect)

            pygame.display.flip()
            clock.tick(FPS)
        except Exception as e:
            error_message = str(e)
            print("GUI: Error in main loop:", error_message)

    pygame.quit()

if __name__ == "__main__":
    # start UDP listener thread
    t = threading.Thread(target=udp_listener, daemon=True)
    t.start()
    run_gui()

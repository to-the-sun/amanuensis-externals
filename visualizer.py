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
    "bar_data": {},
    "logged_hashes": set(),
    "song_reach": 0,
    "bar_length": 125,
    "events": [],
    "smartloop_start": -1,
    "smartloop_end": -1,
    "bar_ratings": {},
    "song_bar_ratings": {},
    "average_rating": 0,
    "min_rating": 0,
    "max_rating": 0,
    "spans_seen": {} # (track_id, first_bar_ts) -> {"rating": float, "bars": []}
}
state_lock = threading.Lock()

def snap_to_bar(ts, bar_length):
    """Snaps a timestamp to the nearest bar start."""
    if bar_length <= 0:
        return float(ts)
    return round(float(ts) / bar_length) * bar_length

def perform_smartloop_analysis():
    """Performs the smartloop calculations locally based on gathered data."""
    # Copy data to minimize lock duration
    with state_lock:
        bar_ratings_copy = {tid: bars.copy() for tid, bars in state["bar_ratings"].items()}
        tracks_copy = {tid: bars[:] for tid, bars in state["tracks"].items()}
        bar_length = state.get("bar_length", 125)

    # Aggregate ratings by timestamp across all tracks
    ts_to_ratings = {} # ts -> [ratings]
    for tid, bars in bar_ratings_copy.items():
        for b_ts, rating in bars.items():
            ts = snap_to_bar(b_ts, bar_length)
            if ts < 0: continue
            if ts not in ts_to_ratings:
                ts_to_ratings[ts] = []
            ts_to_ratings[ts].append(rating)

    if not ts_to_ratings:
        return

    # Calculate averaged rating per timestamp
    song_bars = {} # ts -> averaged_rating
    for ts, ratings in ts_to_ratings.items():
        song_bars[ts] = sum(ratings) / len(ratings)

    all_song_ratings = list(song_bars.values())
    avg = sum(all_song_ratings) / len(all_song_ratings)
    mi = min(all_song_ratings)
    ma = max(all_song_ratings)
    # Store for rendering
    song_bar_ratings = {str(float(ts)): r for ts, r in song_bars.items()}

    print(f"DEBUG: Smartloop Local Analysis. Ratings: min={mi:.2f}, max={ma:.2f}, global_avg={avg:.2f}")

    # Identify above average points and below average intervals (per bar)
    above_avg_points = []
    below_avg_intervals = [] # list of (start, end)

    for ts, rating in song_bars.items():
        if rating > avg:
            above_avg_points.append(ts)
        else:
            below_avg_intervals.append((ts, ts + bar_length))

    sorted_above_avg = sorted(above_avg_points)

    sl_start = -1
    sl_end = -1

    if below_avg_intervals:
        # Find the longest clean interval of below average bars
        all_ts = []
        for track_bars in tracks_copy.values():
            for b in track_bars:
                try:
                    ts = snap_to_bar(b, bar_length)
                    if ts >= 0:
                        all_ts.append(ts)
                except: continue

        if all_ts:
            overall_min = min(all_ts)
            overall_max = max(all_ts) + bar_length

            bounded_above_avg = [overall_min - 1.0] + sorted_above_avg + [overall_max + 1.0]

            max_dist = -1.0
            best_S = -1
            best_E = -1

            for i in range(len(bounded_above_avg) - 1):
                p_low = bounded_above_avg[i]
                p_high = bounded_above_avg[i+1]

                cur_min_S = float('inf')
                cur_max_E = float('-inf')
                found_below_avg = False

                for s_start, s_end in below_avg_intervals:
                    if s_start >= p_low and s_end <= p_high:
                        if s_start < cur_min_S: cur_min_S = s_start
                        if s_end > cur_max_E: cur_max_E = s_end
                        found_below_avg = True

                if found_below_avg:
                    dist = cur_max_E - cur_min_S
                    if dist > max_dist:
                        max_dist = dist
                        best_S = cur_min_S
                        best_E = cur_max_E

            if max_dist >= 0:
                sl_start = best_S
                sl_end = best_E
                print(f"DEBUG: Smartloop Local Analysis. Identified Loop: start={best_S:.2f}, end={best_E:.2f}, duration={max_dist:.2f}")
            else:
                print("DEBUG: Smartloop Local Analysis. No valid loop found.")

    # Update state with results
    with state_lock:
        state["average_rating"] = avg
        state["min_rating"] = mi
        state["max_rating"] = ma
        state["song_bar_ratings"] = song_bar_ratings
        state["smartloop_start"] = sl_start
        state["smartloop_end"] = sl_end

def recalculate_reach():
    """Recalculates state['song_reach'] based on the furthest bar in state['tracks']."""
    max_reach = 0
    bar_length = state.get("bar_length", 125)
    for track_bars in state["tracks"].values():
        for bar_ts in track_bars:
            try:
                snapped_ts = snap_to_bar(bar_ts, bar_length)
                b_ts = float(snapped_ts)
                if b_ts + bar_length > max_reach:
                    max_reach = b_ts + bar_length
            except (ValueError, TypeError):
                continue
    state["song_reach"] = max_reach

def process_packet(text, client_sock=None):
    if not text:
        return
    # Pre-process text to handle multiple JSON objects in one stream
    text = text.replace("} {", "}\n{").replace("}{", "}\n{")
    for line in text.split('\n'):
        line = line.strip()
        if not line: continue

        # DEBUG: Log raw received line
        print(f"DEBUG: TCP Received ({len(line)} chars): {line}")

        try:
            pkt = json.loads(line)
            pkt_type = pkt.get("type")

            if pkt_type == "smartloop":
                print(f"DEBUG: Processing 'smartloop' packet. Keys: {list(pkt.keys())}")
                if pkt.get("event") == "debug":
                    max_inventory = pkt.get("inventory", {})
                    results = []
                    print("\n--- DEBUG INVENTORY COMPARISON ---")
                    with state_lock:
                        local_inventory = state["bar_ratings"]

                        # Check all tracks in Max inventory
                        for tid, m_bars in max_inventory.items():
                            # Normalize local keys for the current track
                            l_bars = local_inventory.get(tid, {})
                            normalized_l_bars = {}
                            for k, v in l_bars.items():
                                try:
                                    normalized_l_bars[str(float(k))] = v
                                except ValueError: continue

                            # Normalize Max keys for this track
                            normalized_m_bars = {}
                            for k, v in m_bars.items():
                                try:
                                    normalized_m_bars[str(float(k))] = v
                                except ValueError: continue

                            for b_key, m_rating in normalized_m_bars.items():
                                l_rating = normalized_l_bars.get(b_key)

                                if l_rating is None:
                                    msg = f"T{tid} Bar {b_key}: Missing in Visualizer"
                                    print(msg)
                                    results.append(msg)
                                elif abs(l_rating - m_rating) > 0.0001:
                                    msg = f"T{tid} Bar {b_key}: Rating mismatch (Max: {m_rating:.4f}, Viz: {l_rating:.4f})"
                                    print(msg)
                                    results.append(msg)

                            # Check for extra bars in this track in Visualizer
                            for b_key in normalized_l_bars:
                                if b_key not in normalized_m_bars:
                                    msg = f"T{tid} Bar {b_key}: Extra in Visualizer (Missing in Max)"
                                    print(msg)
                                    results.append(msg)

                        # Check for tracks in Visualizer that are NOT in Max
                        for tid in local_inventory:
                            if tid not in max_inventory:
                                msg = f"T{tid}: Entire track missing in Max"
                                print(msg)
                                results.append(msg)

                    print(f"Comparison finished. {len(results)} discrepancies found.")
                    print("----------------------------------\n")

                    if client_sock:
                        response = "OK: No discrepancies found." if not results else f"Found {len(results)} discrepancies: " + "; ".join(results[:5]) + ("..." if len(results) > 5 else "")
                        try:
                            client_sock.sendall((response + "\n").encode("utf-8"))
                        except: pass
                    return

                with state_lock:
                    bar_length = state.get("bar_length", 125)
                    if "average" in pkt: state["average_rating"] = pkt["average"]
                    if "min" in pkt: state["min_rating"] = pkt["min"]
                    if "max" in pkt: state["max_rating"] = pkt["max"]
                    if "ratings" in pkt:
                        # Merge ratings
                        for tid, bars in pkt["ratings"].items():
                            if tid not in state["bar_ratings"]:
                                state["bar_ratings"][tid] = {}
                            for b_ts, rating in bars.items():
                                snapped_ts = snap_to_bar(b_ts, bar_length)
                                state["bar_ratings"][tid][str(float(snapped_ts))] = rating
                    if "smartloop_start" in pkt: state["smartloop_start"] = pkt["smartloop_start"]
                    if "smartloop_end" in pkt: state["smartloop_end"] = pkt["smartloop_end"]
                return

            if pkt_type != "crucible":
                print(f"DEBUG: Ignoring packet type '{pkt_type}'")
                continue

            print(f"DEBUG: Processing 'crucible' packet. Keys: {list(pkt.keys())}")

            with state_lock:
                state["bar_length"] = pkt.get("bar_length", state.get("bar_length", 125))

                dirty = False
                if "tracks" in pkt:
                    new_tracks = {}
                    for tid, t_bars in pkt["tracks"].items():
                        new_tracks[tid] = [snap_to_bar(b, state["bar_length"]) for b in t_bars]
                    state["tracks"] = new_tracks

                    if not state["tracks"]:
                        state["bar_data"] = {}
                        state["logged_hashes"].clear()
                        state["bar_ratings"] = {}
                        state["spans_seen"] = {}
                    dirty = True

                if pkt.get("event") == "new_span":
                    track = pkt.get("new_span_track")
                    orig_bars = pkt.get("new_span_bars", [])
                    rating = pkt.get("new_span_rating", 0.0)
                    new_data = pkt.get("new_span_data", {})
                    bar_length = state["bar_length"]

                    # Snap all bar timestamps
                    bars = [snap_to_bar(b, bar_length) for b in orig_bars]

                    # Update local track state from the new span event
                    if track is not None:
                        t_str = str(track)
                        if t_str not in state["tracks"]:
                            state["tracks"][t_str] = []
                        if t_str not in state["bar_data"]:
                            state["bar_data"][t_str] = {}

                        print(f"DEBUG: Storing data for T{t_str}. Bars in event: {bars}. Data keys: {list(new_data.keys())}")

                        existing_bars = set(state["tracks"][t_str])
                        added = False
                        for b in bars:
                            if b not in existing_bars:
                                state["tracks"][t_str].append(b)
                                added = True

                        for b_ts, b_data in new_data.items():
                            snapped_ts = snap_to_bar(b_ts, bar_length)
                            state["bar_data"][t_str][str(float(snapped_ts))] = b_data

                        # Update ratings for analysis
                        if t_str not in state["bar_ratings"]:
                            state["bar_ratings"][t_str] = {}
                        for b in bars:
                            state["bar_ratings"][t_str][str(float(b))] = rating

                        if bars:
                            span_id = (t_str, bars[0])
                            state["spans_seen"][span_id] = {"rating": rating, "bars": bars}

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

                if pkt.get("event") == "replace":
                    track = pkt.get("track")
                    bar = pkt.get("bar")
                    rating = pkt.get("rating")
                    if track is not None and bar is not None and rating is not None:
                        t_str = str(track)
                        bar_length = state["bar_length"]
                        snapped_ts = snap_to_bar(bar, bar_length)
                        b_str = str(float(snapped_ts))
                        if t_str not in state["bar_ratings"]:
                            state["bar_ratings"][t_str] = {}
                        state["bar_ratings"][t_str][b_str] = rating
                        print(f"DEBUG: Replaced rating for T{t_str} bar {b_str} with {rating}")

                if dirty:
                    recalculate_reach()

        except json.JSONDecodeError as e:
            print(f"ERROR: Failed to decode JSON: {e}")
            print(f"ERROR: Raw line causing error: {line[:500]}...")
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
                process_packet(line.strip(), sock)
        except: break
    sock.close()

def smartloop_worker():
    """Background thread that runs the smartloop analysis periodically."""
    print("Smartloop Analysis Worker started.")
    while True:
        try:
            perform_smartloop_analysis()
        except Exception:
            traceback.print_exc()
        time.sleep(1)

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
            tracks = {tid: bars[:] for tid, bars in state["tracks"].items()}
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
        with state_lock:
            avg = state["average_rating"]
            mi = state["min_rating"]
            ma = state["max_rating"]
            song_bar_ratings = state.get("song_bar_ratings", {}).copy()

        for tid, bars in tracks.items():
            if tid not in track_to_row: continue
            row = track_to_row[tid]
            for bar_ts in bars:
                try:
                    snapped_ts = snap_to_bar(bar_ts, bar_length)
                    b_ts = int(snapped_ts)
                except (ValueError, TypeError): continue
                if bar_length <= 0: continue

                # Determine color tint
                color = [80, 80, 100]
                # Use averaged song-level rating for consistency with smartloop analysis
                rating = song_bar_ratings.get(str(float(b_ts)))
                if rating is not None:
                    if rating > avg:
                        # Tint green
                        diff = rating - avg
                        denom = ma - avg if ma > avg else 1
                        factor = min(1.0, diff / denom)
                        # Interpolate (80, 80, 100) -> (80, 200, 80)
                        color[1] = int(80 + (200 - 80) * factor)
                        color[2] = int(100 + (80 - 100) * factor)
                    elif rating < avg:
                        # Tint red
                        diff = avg - rating
                        denom = avg - mi if avg > mi else 1
                        factor = min(1.0, diff / denom)
                        # Interpolate (80, 80, 100) -> (200, 80, 80)
                        color[0] = int(80 + (200 - 80) * factor)
                        color[2] = int(100 + (80 - 100) * factor)

                col = b_ts // bar_length
                rect = pygame.Rect(margin_left + col * cell_w + 1, margin_top + row * cell_h + 1, cell_w - 1, cell_h - 1)
                pygame.draw.rect(screen, tuple(color), rect)

        # Draw note hash marks
        with state_lock:
            bar_data = {tid: bars.copy() for tid, bars in state["bar_data"].items()}

        for tid, bars_info in bar_data.items():
            if tid not in track_to_row: continue
            row = track_to_row[tid]
            row_y_bottom = margin_top + (row + 1) * cell_h

            for b_ts, info in bars_info.items():
                absolutes = info.get("absolutes")
                scores = info.get("scores")
                offset = info.get("offset")

                if absolutes is None or scores is None or offset is None:
                    # Log once per bar if data is missing
                    if (tid, b_ts, "missing") not in state["logged_hashes"]:
                        print(f"DEBUG: Skipping bar {b_ts} on T{tid} - missing data (abs:{type(absolutes)}, scores:{type(scores)}, off:{type(offset)})")
                        state["logged_hashes"].add((tid, b_ts, "missing"))
                    continue

                # Ensure all are lists
                if not isinstance(absolutes, list): absolutes = [absolutes]
                if not isinstance(scores, list): scores = [scores]
                if not isinstance(offset, list): offset = [offset]

                for i in range(len(absolutes)):
                    try:
                        abs_val = float(absolutes[i])
                        score_val = float(scores[i])
                        # Use i-th offset if available, else fallback to first
                        off_val = float(offset[i]) if i < len(offset) else float(offset[0])
                    except (ValueError, TypeError, IndexError) as err:
                        if (tid, b_ts, i, "error") not in state["logged_hashes"]:
                            print(f"DEBUG: Error parsing note {i} in bar {b_ts} on T{tid}: {err}")
                            state["logged_hashes"].add((tid, b_ts, i, "error"))
                        continue

                    if bar_length <= 0: continue

                    # Unique key to prevent console spam
                    hash_key = (tid, b_ts, i)
                    # x position relative to start of song
                    # Calculation per user: (absolute - offset).
                    # This gives song-relative timestamp (assuming offset is standard transcript offset).
                    # Map this ms value to pixels along the grid:
                    rel_ms = abs_val - off_val
                    x_pos = margin_left + (rel_ms / bar_length) * cell_w

                    with state_lock:
                        if hash_key not in state["logged_hashes"]:
                            print(f"[Hash Mark T{tid}] {abs_val:.2f} (absolute) - {off_val:.2f} (offset) = {rel_ms:.2f} (relative) | bar_len: {bar_length}, cell_w: {cell_w:.2f}, x_pos: {x_pos:.2f}")
                            state["logged_hashes"].add(hash_key)

                    # Height relative to score (0.0 to 2.0)
                    clipped_score = max(0.0, min(2.0, score_val))
                    # Map 0.0 -> bottom, 2.0 -> top of 20px row
                    h_px = (clipped_score / 2.0) * cell_h

                    pygame.draw.line(screen, (255, 255, 255), (x_pos, row_y_bottom), (x_pos, row_y_bottom - h_px), 1)

        # Draw Smartloop Range Box
        with state_lock:
            sl_s = state["smartloop_start"]
            sl_e = state["smartloop_end"]

        if sl_s >= 0 and sl_e > sl_s:
            x_s = margin_left + (sl_s / bar_length) * cell_w
            x_e = margin_left + (sl_e / bar_length) * cell_w
            box_rect = pygame.Rect(x_s, margin_top, x_e - x_s, num_tracks * cell_h)
            pygame.draw.rect(screen, (255, 255, 255), box_rect, 2)

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
    threading.Thread(target=smartloop_worker, daemon=True).start()
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

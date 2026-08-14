import pyray as pr
import socket
import json
import threading
import time
import sys
import os
import traceback
import argparse

# Configuration
parser = argparse.ArgumentParser()
parser.add_argument('--port', type=int, default=9001)
parser.add_argument('--group', type=str, default="")
args, unknown = parser.parse_known_args()

TCP_PORT = args.port
GROUP_NAME = args.group
FPS = 60
W, H = 1200, 1000

# Connection Tracking
active_connections = 0
connections_lock = threading.Lock()

# Set headless environment options if requested
if os.environ.get('HEADLESS'):
    pr.set_config_flags(pr.FLAG_WINDOW_HIDDEN)

# Global Visualizer State
state = {
    'clients': {},             # client_id -> client state dictionary
    'times': [],               # Rolling timestamps (last 25 seconds) - fallback
    'onset_envs': [[],[],[],[]], # 4 bands flux - fallback
    'smooth_envs': [[],[],[],[]], # 4 bands smooth - fallback
    'prominences': [[],[],[],[]], # 4 bands prominence - fallback
    'rolling_smoothing_avgs': [0.0]*4,
    'rolling_global_smoothing_avg': 0.0,
    'rating': 0.0,
    'overall_rating': 0.0,
    'std_dev': 0.0,
    'contrast': 0.0,
    'stability': 0.0,
    'max_peak_value': 1.0,
    'min_score_seen': -5.0,
    'max_score_seen': 5.0,
    'tolerance': 29.0,
    'highest_peak_ms': -999.0,
    'peaks': [],
    'accumulated_buffer': [0.0]*5001,
    'current_time': 0.0,
    'exit_flag': False
}
state_lock = threading.Lock()

def process_packet(line):
    line = line.strip()
    if not line: return
    try:
        pkt = json.loads(line)
        if pkt.get('type') != 'analyze':
            return

        with state_lock:
            client_id = pkt.get('client_id', 'default')
            if client_id not in state['clients']:
                state['clients'][client_id] = {
                    'times': [],
                    'onset_envs': [[],[],[],[]],
                    'smooth_envs': [[],[],[],[]],
                    'prominences': [[],[],[],[]],
                    'peaks': [],
                    'rolling_smoothing_avgs': [0.0]*4,
                    'rolling_global_smoothing_avg': 0.0,
                    'rating': 0.0,
                    'overall_rating': 0.0,
                    'std_dev': 0.0,
                    'contrast': 0.0,
                    'stability': 0.0,
                    'max_peak_value': 1.0,
                    'min_score_seen': -5.0,
                    'max_score_seen': 5.0,
                    'tolerance': 29.0,
                    'highest_peak_ms': -999.0,
                    'accumulated_buffer': [0.0]*5001,
                    'current_time': 0.0,
                    'last_update': time.time()
                }

            client = state['clients'][client_id]
            client['last_update'] = time.time()

            # Playhead time
            current_time = pkt.get('time', 0.0)
            client['current_time'] = current_time

            # Recompute timestamps for the 100 incoming frames
            # 1ms frame duration
            chunk_times = [current_time - (99 - i) * 0.001 for i in range(100)]
            client['times'].extend(chunk_times)

            # Envelopes
            flux_chunk = pkt.get('flux', [])
            smooth_chunk = pkt.get('smooth', [])
            prom_chunk = pkt.get('prominence', [])

            for b in range(4):
                if b < len(flux_chunk):
                    client['onset_envs'][b].extend(flux_chunk[b])
                if b < len(smooth_chunk):
                    client['smooth_envs'][b].extend(smooth_chunk[b])
                if b < len(prom_chunk):
                    client['prominences'][b].extend(prom_chunk[b])

            # Prune histories older than 25 seconds
            max_history = 25000
            if len(client['times']) > max_history:
                pop_count = len(client['times']) - max_history
                client['times'] = client['times'][pop_count:]
                for b in range(4):
                    client['onset_envs'][b] = client['onset_envs'][b][pop_count:]
                    client['smooth_envs'][b] = client['smooth_envs'][b][pop_count:]
                    client['prominences'][b] = client['prominences'][b][pop_count:]

            # Metrics and averages
            client['rolling_smoothing_avgs'] = pkt.get('smoothing_avgs', [0.0]*4)
            client['rolling_global_smoothing_avg'] = pkt.get('global_smoothing_avg', 0.0)
            client['rating'] = pkt.get('rating', 0.0)
            client['overall_rating'] = pkt.get('rating', 0.0)
            client['std_dev'] = pkt.get('std_dev', 0.0)
            client['contrast'] = pkt.get('contrast', 0.0)
            client['stability'] = pkt.get('stability', 0.0)
            client['max_peak_value'] = pkt.get('max_peak_value', 1.0)
            client['min_score_seen'] = pkt.get('min_score_seen', -5.0)
            client['max_score_seen'] = pkt.get('max_score_seen', 5.0)
            client['tolerance'] = pkt.get('tolerance', 29.0)
            client['highest_peak_ms'] = pkt.get('highest_peak_ms', -999.0)
            client['accumulated_buffer'] = pkt.get('accumulated_buffer', [0.0]*5001)

            # Append new peaks
            new_peaks = pkt.get('peaks', [])
            client['peaks'].extend(new_peaks)

            # Prune peaks older than 30 seconds to avoid memory growth
            client['peaks'] = [p for p in client['peaks'] if current_time - p.get('time', 0.0) <= 30.0]

            # Set the latest current_time globally
            state['current_time'] = max(state['current_time'], current_time)

    except Exception as e:
        print(f"Error parsing packet: {e}", file=sys.stderr)

def tcp_server():
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("", TCP_PORT))
    except Exception as e:
        print(f"ERROR: Failed to bind to port {TCP_PORT}: {e}", file=sys.stderr)
        sys.exit(1)
    server_sock.listen(5)
    print(f"Cumulative Transience Companion Visualizer ({GROUP_NAME if GROUP_NAME else 'Default Group'}): Listening on port {TCP_PORT}", flush=True)

    while True:
        try:
            client_sock, addr = server_sock.accept()
            print(f"Accepted connection from {addr}", flush=True)
            threading.Thread(target=handle_client, args=(client_sock,), daemon=True).start()
        except Exception:
            break

def handle_client(sock):
    global active_connections
    with connections_lock:
        active_connections += 1

    buffer = ""
    while True:
        try:
            data = sock.recv(65536)
            if not data:
                break
            buffer += data.decode("utf-8", errors="replace")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                process_packet(line.strip())
        except Exception:
            break

    sock.close()

    with connections_lock:
        active_connections -= 1
        if active_connections <= 0:
            print("All clients disconnected, exiting...", flush=True)
            with state_lock:
                state['exit_flag'] = True
        else:
            print(f"A client disconnected. {active_connections} clients remaining.", flush=True)

def run_gui():
    import raylib_renderer

    title_str = f"Cumulative Transience Real-Time Visualizer ({GROUP_NAME})" if GROUP_NAME else "Cumulative Transience Real-Time Visualizer"
    pr.init_window(W, H, title_str)
    pr.set_target_fps(FPS)

    while not pr.window_should_close():
        with state_lock:
            if state['exit_flag']:
                break

            # Prune/Clean stale clients (stale for more than 5 seconds)
            now = time.time()
            active_clients = [c for c in state['clients'].values() if now - c['last_update'] < 5.0]

            if not active_clients:
                frame_data = {
                    'times': list(state['times']),
                    'onset_envs': [list(x) for x in state['onset_envs']],
                    'smooth_envs': [list(x) for x in state['smooth_envs']],
                    'prominences': [list(x) for x in state['prominences']],
                    'rolling_smoothing_avgs': list(state['rolling_smoothing_avgs']),
                    'rolling_global_smoothing_avg': state['rolling_global_smoothing_avg'],
                    'rating': state['rating'],
                    'overall_rating': state['overall_rating'],
                    'std_dev': state['std_dev'],
                    'contrast': state['contrast'],
                    'stability': state['stability'],
                    'max_peak_value': state['max_peak_value'],
                    'min_score_seen': state['min_score_seen'],
                    'max_score_seen': state['max_score_seen'],
                    'tolerance': state['tolerance'],
                    'highest_peak_ms': state['highest_peak_ms'],
                    'peaks': list(state['peaks']),
                    'accumulated_buffer': list(state['accumulated_buffer']),
                    'title': title_str
                }
                current_time = state['current_time']
            elif len(active_clients) == 1:
                ref_client = active_clients[0]
                frame_data = {
                    'times': list(ref_client['times']),
                    'onset_envs': [list(x) for x in ref_client['onset_envs']],
                    'smooth_envs': [list(x) for x in ref_client['smooth_envs']],
                    'prominences': [list(x) for x in ref_client['prominences']],
                    'rolling_smoothing_avgs': list(ref_client['rolling_smoothing_avgs']),
                    'rolling_global_smoothing_avg': ref_client['rolling_global_smoothing_avg'],
                    'rating': ref_client['rating'],
                    'overall_rating': ref_client['overall_rating'],
                    'std_dev': ref_client['std_dev'],
                    'contrast': ref_client['contrast'],
                    'stability': ref_client['stability'],
                    'max_peak_value': ref_client['max_peak_value'],
                    'min_score_seen': ref_client['min_score_seen'],
                    'max_score_seen': ref_client['max_score_seen'],
                    'tolerance': ref_client['tolerance'],
                    'highest_peak_ms': ref_client['highest_peak_ms'],
                    'peaks': list(ref_client['peaks']),
                    'accumulated_buffer': list(ref_client['accumulated_buffer']),
                    'title': title_str
                }
                current_time = ref_client['current_time']
            else:
                ref_client = max(active_clients, key=lambda c: c['last_update'])
                ref_times = ref_client['times']
                num_points = len(ref_times)

                merged_onset = [list(ref_client['onset_envs'][b]) for b in range(4)]
                merged_smooth = [list(ref_client['smooth_envs'][b]) for b in range(4)]
                merged_prom = [list(ref_client['prominences'][b]) for b in range(4)]

                for client in active_clients:
                    if client is ref_client:
                        continue
                    for b in range(4):
                        c_len = len(client['times'])
                        limit = min(num_points, c_len)
                        if limit > 0:
                            # Keep prefix unchanged, merge the suffix with element-wise max
                            prefix_onset = merged_onset[b][:-limit]
                            merged_onset[b] = prefix_onset + [max(x, y) for x, y in zip(merged_onset[b][-limit:], client['onset_envs'][b][-limit:])]

                            prefix_smooth = merged_smooth[b][:-limit]
                            merged_smooth[b] = prefix_smooth + [max(x, y) for x, y in zip(merged_smooth[b][-limit:], client['smooth_envs'][b][-limit:])]

                            prefix_prom = merged_prom[b][:-limit]
                            merged_prom[b] = prefix_prom + [max(x, y) for x, y in zip(merged_prom[b][-limit:], client['prominences'][b][-limit:])]

                merged_peaks = []
                for client in active_clients:
                    merged_peaks.extend(client['peaks'])
                merged_peaks.sort(key=lambda p: p.get('time', 0.0))

                frame_data = {
                    'times': list(ref_times),
                    'onset_envs': merged_onset,
                    'smooth_envs': merged_smooth,
                    'prominences': merged_prom,
                    'rolling_smoothing_avgs': list(ref_client['rolling_smoothing_avgs']),
                    'rolling_global_smoothing_avg': ref_client['rolling_global_smoothing_avg'],
                    'rating': ref_client['rating'],
                    'overall_rating': ref_client['overall_rating'],
                    'std_dev': ref_client['std_dev'],
                    'contrast': ref_client['contrast'],
                    'stability': ref_client['stability'],
                    'max_peak_value': ref_client['max_peak_value'],
                    'min_score_seen': ref_client['min_score_seen'],
                    'max_score_seen': ref_client['max_score_seen'],
                    'tolerance': ref_client['tolerance'],
                    'highest_peak_ms': ref_client['highest_peak_ms'],
                    'peaks': merged_peaks,
                    'accumulated_buffer': list(ref_client['accumulated_buffer']),
                    'title': title_str
                }
                current_time = ref_client['current_time']

        pr.begin_drawing()
        raylib_renderer.draw_renderer(W, H, current_time, frame_data)
        pr.end_drawing()

    pr.close_window()
    sys.exit(0)

if __name__ == "__main__":
    # Start TCP server in background
    threading.Thread(target=tcp_server, daemon=True).start()
    run_gui()

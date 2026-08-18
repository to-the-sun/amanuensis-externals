import pyray as pr
import socket
import json
import threading
import time
import sys
import os
import argparse
import traceback

# Parse CLI arguments
parser = argparse.ArgumentParser(description="Cumulative Transience Real-Time Visualizer")
parser.add_argument('--port', type=int, default=9001, help="TCP port to listen on")
parser.add_argument('--group', type=str, default="", help="Shared group name")
parser.add_argument('--channel', type=int, default=-1, help="Channel index (-1 for single channel)")
parser.add_argument('--name', type=str, default="", help="Object scripting name")
parser.add_argument('--log', type=int, default=0, help="Enable TCP packet logging console")
args, _ = parser.parse_known_args()

TCP_PORT = args.port
INITIAL_GROUP = args.group
INITIAL_CHANNEL = args.channel
INITIAL_NAME = args.name
INITIAL_LOG = args.log

FPS = 60
W, H = 1200, 1000

if os.environ.get('HEADLESS'):
    pr.set_config_flags(pr.FLAG_WINDOW_HIDDEN)

# Global Visualizer State
state = {
    'group': INITIAL_GROUP,
    'channel': INITIAL_CHANNEL,
    'scripting_name': INITIAL_NAME,
    'log_enabled': INITIAL_LOG,
    'packet_logs': [],
    'times': [],               # Rolling timestamps (last 25 seconds)
    'onset_envs': [[],[],[],[]], # 4 bands flux
    'smooth_envs': [[],[],[],[]], # 4 bands smooth
    'prominences': [[],[],[],[]], # 4 bands prominence
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

    with state_lock:
        state['packet_logs'].append(line)
        if len(state['packet_logs']) > 50:
            state['packet_logs'] = state['packet_logs'][-50:]

    try:
        pkt = json.loads(line)
        pkt_type = pkt.get('type')
        if pkt_type not in ('analyze', 'mc_analyze'):
            return

        with state_lock:
            if 'log' in pkt and pkt['log'] is not None:
                state['log_enabled'] = pkt['log']
            if 'group' in pkt and pkt['group']:
                state['group'] = pkt['group']
            if 'channel' in pkt and pkt['channel'] is not None:
                state['channel'] = pkt['channel']
            if 'scripting_name' in pkt and pkt['scripting_name'] is not None:
                state['scripting_name'] = pkt['scripting_name']

            current_time = pkt.get('time', 0.0)
            state['current_time'] = current_time

            chunk_times = [current_time - (99 - i) * 0.001 for i in range(100)]
            state['times'].extend(chunk_times)

            flux_chunk = pkt.get('flux', [])
            smooth_chunk = pkt.get('smooth', [])
            prom_chunk = pkt.get('prominence', [])

            for b in range(4):
                if b < len(flux_chunk):
                    state['onset_envs'][b].extend(flux_chunk[b])
                if b < len(smooth_chunk):
                    state['smooth_envs'][b].extend(smooth_chunk[b])
                if b < len(prom_chunk):
                    state['prominences'][b].extend(prom_chunk[b])

            max_history = 25000
            if len(state['times']) > max_history:
                pop_count = len(state['times']) - max_history
                state['times'] = state['times'][pop_count:]
                for b in range(4):
                    state['onset_envs'][b] = state['onset_envs'][b][pop_count:]
                    state['smooth_envs'][b] = state['smooth_envs'][b][pop_count:]
                    state['prominences'][b] = state['prominences'][b][pop_count:]

            state['rolling_smoothing_avgs'] = pkt.get('smoothing_avgs', [0.0]*4)
            state['rolling_global_smoothing_avg'] = pkt.get('global_smoothing_avg', 0.0)
            state['rating'] = pkt.get('rating', 0.0)
            state['overall_rating'] = pkt.get('rating', 0.0)
            state['std_dev'] = pkt.get('std_dev', 0.0)
            state['contrast'] = pkt.get('contrast', 0.0)
            state['stability'] = pkt.get('stability', 0.0)
            state['max_peak_value'] = pkt.get('max_peak_value', 1.0)
            state['min_score_seen'] = pkt.get('min_score_seen', -5.0)
            state['max_score_seen'] = pkt.get('max_score_seen', 5.0)
            state['tolerance'] = pkt.get('tolerance', 29.0)
            state['highest_peak_ms'] = pkt.get('highest_peak_ms', -999.0)
            state['accumulated_buffer'] = pkt.get('accumulated_buffer', [0.0]*5001)

            new_peaks = pkt.get('peaks', [])
            state['peaks'].extend(new_peaks)
            state['peaks'] = [p for p in state['peaks'] if current_time - p.get('time', 0.0) <= 30.0]

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
    print(f"Cumulative Transience Companion Visualizer: Listening on port {TCP_PORT}", flush=True)

    while True:
        try:
            client_sock, addr = server_sock.accept()
            print(f"Accepted connection from {addr}", flush=True)
            threading.Thread(target=handle_client, args=(client_sock,), daemon=True).start()
        except Exception:
            break

def handle_client(sock):
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
    print("Client disconnected, exiting...", flush=True)
    sock.close()
    with state_lock:
        state['exit_flag'] = True

def build_title(scripting_name, channel, port):
    prefix_parts = []
    if scripting_name:
        prefix_parts.append(scripting_name)
    if channel >= 0:
        ch_1based = channel + 1
        prefix_parts.append(f"Ch {ch_1based}")

    if prefix_parts:
        prefix_str = " ".join(prefix_parts) + ", "
    else:
        prefix_str = ""

    return f"Cumulative Transience Real-Time Visualizer ({prefix_str}Port {port})"

def run_gui():
    import raylib_renderer

    with state_lock:
        s_name = state['scripting_name']
        ch_idx = state['channel']

    win_title = build_title(s_name, ch_idx, TCP_PORT)

    pr.set_config_flags(pr.FLAG_WINDOW_MINIMIZED)
    pr.init_window(W, H, win_title)
    raylib_renderer.get_font()
    pr.set_target_fps(FPS)

    current_title = win_title

    while not pr.window_should_close():
        with state_lock:
            if state['exit_flag']:
                break

            s_name = state['scripting_name']
            ch_idx = state['channel']
            win_title = build_title(s_name, ch_idx, TCP_PORT)

            frame_data = {
                'group': state['group'],
                'channel': state['channel'],
                'scripting_name': state['scripting_name'],
                'log_enabled': state['log_enabled'],
                'packet_logs': list(state['packet_logs']),
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
                'title': win_title
            }
            current_time = state['current_time']

        if win_title != current_title:
            pr.set_window_title(win_title)
            current_title = win_title

        pr.begin_drawing()
        raylib_renderer.draw_renderer(W, H, current_time, frame_data)
        pr.end_drawing()

    pr.close_window()
    sys.exit(0)

if __name__ == "__main__":
    threading.Thread(target=tcp_server, daemon=True).start()
    run_gui()

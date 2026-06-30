import argparse
import librosa
import numpy as np
import scipy.signal
import os
import cv2
from moviepy import VideoClip, AudioFileClip
from tqdm import tqdm
import subprocess
import tempfile
import shutil
import traceback
import sys
import threading
try:
    import ct_utils
except ImportError:
    sys.path.append(r'D:\[Library]\[Documents]\Max 8\Library\analyze~\python')
    import ct_utils

try:
    import static_ffmpeg
    static_ffmpeg.add_paths()
except ImportError:
    pass

_initialized = False
_init_lock = threading.Lock()
cumulative_transience = None

def ensure_initialized():
    """Ensures that the extension is built and imported."""
    global _initialized, cumulative_transience
    if _initialized:
        return

    with _init_lock:
        if _initialized:
            return

        # Ensure built before attempt import
        ct_utils.ensure_extension_built()
        try:
            import cumulative_transience as ct
            cumulative_transience = ct
        except ImportError:
            fallback_path = r'D:\[Library]\[Documents]\Max 8\Library\analyze~\python'
            if fallback_path not in sys.path:
                sys.path.append(fallback_path)
                try:
                    import cumulative_transience as ct
                    cumulative_transience = ct
                except ImportError:
                    cumulative_transience = None
            else:
                cumulative_transience = None
        _initialized = True

def hex_to_bgr(hex_str):
    hex_str = hex_str.lstrip('#')
    lv = len(hex_str)
    rgb = tuple(int(hex_str[i:i + lv // 3], 16) for i in range(0, lv, lv // 3))
    return (rgb[2], rgb[1], rgb[0])

def get_score_color(score, min_score, max_score):
    """
    Returns a BGR color tuple based on the resonance score relative to min/max seen.
    Red (#ff0000) -> Gray (#808080) -> Green (#00ff00).
    """
    if score == 0: return (128, 128, 128)
    if score < 0:
        t = score / min_score if min_score < 0 else 0.0
        t = max(0, min(1, t))
        # Gray (128,128,128) to Red (0,0,255)
        return (int(128 * (1 - t)), int(128 * (1 - t)), int(128 + 127 * t))
    else:
        t = score / max_score if max_score > 0 else 0.0
        t = max(0, min(1, t))
        # Gray (128,128,128) to Green (0,255,0)
        return (int(128 * (1 - t)), int(128 + 127 * t), int(128 * (1 - t)))

def draw_dashed_line(img, pt1, pt2, color, thickness=1, dash_length=10):
    dist = np.sqrt((pt1[0] - pt2[0])**2 + (pt1[1] - pt2[1])**2)
    dashes = int(dist / dash_length)
    for i in range(dashes):
        start = [int(pt1[0] + (pt2[0] - pt1[0]) * i / dashes), int(pt1[1] + (pt2[1] - pt1[1]) * i / dashes)]
        end = [int(pt1[0] + (pt2[0] - pt1[0]) * (i + 0.5) / dashes), int(pt1[1] + (pt2[1] - pt1[1]) * (i + 0.5) / dashes)]
        cv2.line(img, tuple(start), tuple(end), color, thickness)

def draw_dotted_line(img, pt1, pt2, color, thickness=1, gap=5):
    dist = np.sqrt((pt1[0] - pt2[0])**2 + (pt1[1] - pt2[1])**2)
    dots = int(dist / gap)
    for i in range(dots):
        pt = [int(pt1[0] + (pt2[0] - pt1[0]) * i / dots), int(pt1[1] + (pt2[1] - pt1[1]) * i / dots)]
        cv2.circle(img, tuple(pt), thickness, color, -1)

def generate_video(audio_path, data):
    """
    Generates a video file using OpenCV for high-efficiency rendering.
    """
    ensure_initialized()
    if cumulative_transience is None:
        raise ImportError("The 'cumulative_transience' extension module could not be loaded.")

    print(f"Generating video for {audio_path}...")
    try:
        times = np.array(data['times'])
        onset_envs = data['onset_envs']
        rolling_thresholds = data['rolling_thresholds']
        peak_indices_list = data['peaks_list']
        max_peak = data['max_peak_value']
        all_valid_peak_indices = set().union(*peak_indices_list)

        # Video parameters
        width, height = 1280, 1440
        fps = 30
        duration = times[-1]
        num_frames = int(duration * fps)

        # Layout (Y offsets)
        ZONE1_H, ZONE2_H, ZONE3_H = 600, 240, 600
        Z1_TOP, Z2_TOP, Z3_TOP = 0, ZONE1_H, ZONE1_H + ZONE2_H

        # Exact Colors from Matplotlib (converted to BGR)
        BAND_COLORS = [hex_to_bgr(c) for c in ['#1b4f72', '#3498db', '#2ecc71', '#a9dfbf']]
        PLAYHEAD_C = hex_to_bgr('#e67e22')
        CLEANUP_C = hex_to_bgr('#9b59b6')
        PEAK_C = hex_to_bgr('#e74c3c')
        BUFFER_C = hex_to_bgr('#f1c40f')
        MEAN_C = (128, 128, 128)
        GRID_C = (50, 50, 50)
        BG_C = (15, 15, 15)
        TEXT_C = hex_to_bgr('#f1c40f')

        analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=max_peak, sr=data.get('sample_rate', 44100))

        all_processed_peaks = []
        for p_idx in tqdm(sorted(all_valid_peak_indices), desc="Pre-processing Peaks", unit="peak"):
             res_list = analyzer.process_new_peaks(p_idx, peak_indices_list, onset_envs, all_valid_peak_indices, times)
             all_processed_peaks.extend(res_list)

        # Pre-calculate static background elements
        base_bg = np.full((height, width, 3), BG_C, dtype=np.uint8)

        # Grid lines and Zone borders
        for x in range(0, width, 100): cv2.line(base_bg, (x, 0), (x, height), GRID_C, 1)
        for y in range(0, height, 100): cv2.line(base_bg, (0, y), (width, y), GRID_C, 1)
        cv2.line(base_bg, (0, Z2_TOP), (width, Z2_TOP), (100, 100, 100), 2)
        cv2.line(base_bg, (0, Z3_TOP), (width, Z3_TOP), (100, 100, 100), 2)

        # Static Labels
        font = cv2.FONT_HERSHEY_DUPLEX
        cv2.putText(base_bg, f"4-Band Analysis - {os.path.basename(audio_path)}", (width//2 - 300, 40), font, 1.0, (200, 200, 200), 1, cv2.LINE_AA)
        cv2.putText(base_bg, "39ms Rolling Window Snapshot", (width//2 - 250, Z2_TOP + 40), font, 1.2, (255, 255, 255), 2, cv2.LINE_AA)
        cv2.putText(base_bg, "Accumulated 5s Historical Buffer", (width//2 - 250, Z3_TOP + 40), font, 1.0, (200, 200, 200), 1, cv2.LINE_AA)

        # Snapshot Lanes Labels
        lane_labels = ['Sub', 'Bass', 'Mid', 'Hi']
        for i in range(4):
            sy = Z2_TOP + 80 + i * 50
            cv2.putText(base_bg, lane_labels[i], (20, sy + 5), font, 0.7, (150, 150, 150), 1, cv2.LINE_AA)
            cv2.line(base_bg, (80, sy), (width-20, sy), (40, 40, 40), 1)

        POPUP_LIFETIME = 60
        active_flashes = [] # [snapshot, lifetime]
        active_scores = []  # [text, time, val, peak_val, lifetime]
        active_qualifiers = [] # [ms, val, lifetime]
        rolling_window_scores = []
        last_f_idx = -1
        current_snapshot_avg = 0.0

        def make_frame(t):
            nonlocal last_f_idx, current_snapshot_avg, rolling_window_scores
            frame = base_bg.copy()
            f_idx = np.searchsorted(times, t)

            # Catch up state logic
            all_new_peak_data = [p for p in all_processed_peaks if p['p_idx'] > last_f_idx and p['p_idx'] <= f_idx]
            for p in all_new_peak_data:
                rolling_window_scores.append({'frame': p['p_idx'], 'score': p['total_score'], 'band_idx': p['band_idx']})
                active_flashes.append([p['snapshot'], p['total_score'], POPUP_LIFETIME])
                active_scores.append([f"{p['total_score']:+.2f}", p['time'], p['total_score'], p['peak_val'], POPUP_LIFETIME])
                active_qualifiers.clear()
                for q in p['qualifiers']:
                    active_qualifiers.append([q['ms'], q['val'], POPUP_LIFETIME])

            metrics = analyzer.update_metrics(f_idx)
            rolling_window_scores = [s for s in rolling_window_scores if s['frame'] > f_idx - 39]
            if rolling_window_scores:
                current_snapshot_avg = sum(s['score'] for s in rolling_window_scores) / len(rolling_window_scores)

            # --- Zone 1: Transient View ---
            t_start, t_end = t - 20, t + 5
            env_scale = max_peak * 1.1 + 1e-6
            def t_to_x(val): return ((np.asarray(val) - t_start) / 25.0 * width).astype(np.int32)
            def val_to_y(val): return (ZONE1_H - (np.asarray(val) / env_scale * ZONE1_H * 0.8)).astype(np.int32)

            for b in range(4):
                env = np.asarray(onset_envs[b])
                mask = (times >= t_start) & (times <= t_end)
                if np.any(mask):
                    x_pts = ((times[mask] - t_start) / 25.0 * width).astype(np.int32)
                    y_pts = val_to_y(env[mask])
                    pts = np.column_stack((x_pts, y_pts))
                    if len(pts) > 1: cv2.polylines(frame, [pts], False, BAND_COLORS[b], 2, cv2.LINE_AA)

                # Threshold line (dashed-ish)
                thresh_y = val_to_y(rolling_thresholds[b][f_idx])
                draw_dashed_line(frame, (0, thresh_y), (width, thresh_y), BAND_COLORS[b], 1, 20)

                # Peaks markers ('x')
                p_indices = [idx for idx in peak_indices_list[b] if times[idx] >= t_start and times[idx] <= t_end]
                for p_i in p_indices:
                    px = t_to_x(times[p_i])
                    py = val_to_y(env[p_i])
                    cv2.line(frame, (px-5, py-5), (px+5, py+5), PEAK_C, 1)
                    cv2.line(frame, (px-5, py+5), (px+5, py-5), PEAK_C, 1)

            # Playhead & Cleanup
            draw_dashed_line(frame, (t_to_x(t), 0), (t_to_x(t), ZONE1_H), PLAYHEAD_C, 2, 15)
            draw_dotted_line(frame, (t_to_x(t-15), 0), (t_to_x(t-15), ZONE1_H), CLEANUP_C, 2, 8)
            
            # Floating Scores
            for s in active_scores[:]:
                txt, p_time, val, p_val, life = s
                alpha = life / 60.0
                c = get_score_color(val, metrics['min_score_seen'], metrics['max_score_seen'])
                y_pos = int(val_to_y(p_val) - (1-alpha)*100)
                cv2.putText(frame, txt, (t_to_x(p_time)-25, y_pos), font, 0.6, c, 1, cv2.LINE_AA)
                s[4] -= 1
                if s[4] <= 0: active_scores.remove(s)

            # --- Zone 2: Snapshot View ---
            if rolling_window_scores:
                latest_p = max(s['frame'] for s in rolling_window_scores)
                for s in rolling_window_scores:
                    rel_ms = float(s['frame'] - latest_p)
                    # Snapshot x-axis: [-45, 1]. Anchor latest peak (x=0) at width*0.9
                    sx = int( (rel_ms + 45) / 46.0 * (width - 150) + 100 )
                    sy = Z2_TOP + 80 + s['band_idx'] * 50
                    c = get_score_color(s['score'], metrics['min_score_seen'], metrics['max_score_seen'])
                    cv2.line(frame, (sx, sy-20), (sx, sy+20), BAND_COLORS[s['band_idx']], 4)
                    cv2.putText(frame, f"{s['score']:+.2f}", (sx - 80, sy + 8), font, 0.7, c, 2, cv2.LINE_AA)

            # --- Zone 3: Buffer View ---
            buf = analyzer.accumulated_buffer
            buf_max = np.max(buf[:-99]) if len(buf) > 99 else 1.0
            b_scale = buf_max * 1.1 + 1e-6
            def b_to_x(ms): return ((np.asarray(ms) + 5000) / 5000.0 * width).astype(np.int32)
            def b_to_y(val): return (height - (np.asarray(val) / b_scale * ZONE3_H * 0.9) - 50).astype(np.int32)

            # Fading Flashes
            overlay = frame.copy()
            for f in active_flashes[:]:
                snap, score, life = f
                alpha = (life / 60.0) * 0.5
                c = get_score_color(score, metrics['min_score_seen'], metrics['max_score_seen'])
                pts = [[b_to_x(i - 5000), b_to_y(snap[i])] for i in range(0, 5001, 20)]
                pts.append([width, height-50]); pts.append([0, height-50])
                cv2.fillPoly(overlay, [np.array(pts, np.int32)], c)
                f[2] -= 1
                if f[2] <= 0: active_flashes.remove(f)
            cv2.addWeighted(overlay, 0.4, frame, 0.6, 0, frame)

            # Buffer line
            b_pts = np.array([[b_to_x(i - 5000), b_to_y(buf[i])] for i in range(0, 5001, 5)], np.int32)
            cv2.polylines(frame, [b_pts], False, BUFFER_C, 2, cv2.LINE_AA)

            # Mean line (dashed)
            my = b_to_y(metrics['mean'])
            draw_dashed_line(frame, (0, my), (width, my), MEAN_C, 1, 15)

            # Qualifiers
            for q in active_qualifiers[:]:
                ms, val, life = q
                qx = b_to_x(ms)
                qc = get_score_color(val, -1.0, 1.0)
                draw_dotted_line(frame, (qx, Z3_TOP + 50), (qx, height - 50), qc, 2, 10)
                cv2.putText(frame, f"{val:+.2f}", (qx + 5, Z3_TOP + 150), font, 0.5, qc, 1, cv2.LINE_AA)
                q[2] -= 1
                if q[2] <= 0: active_qualifiers.remove(q)

            # Labels and Metrics
            cv2.putText(frame, f"Rating: {metrics['rating']:.2f}", (50, 80), font, 1.3, TEXT_C, 2, cv2.LINE_AA)
            cv2.putText(frame, f"Score: {current_snapshot_avg:+.2f}", (50, 130), font, 1.1, get_score_color(current_snapshot_avg, metrics['min_score_seen'], metrics['max_score_seen']), 2, cv2.LINE_AA)
            m_txt = f"Std Dev: {metrics['std_dev']:.3f} | Contrast: {metrics['contrast']:.3f} | Peak Std: {metrics['peak_std']:.3f}"
            cv2.putText(frame, m_txt, (50, height - 20), font, 0.8, TEXT_C, 1, cv2.LINE_AA)

            last_f_idx = f_idx
            return cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)

        # Generate Audio for MoviePy
        audio_clip = AudioFileClip(audio_path)
        video_clip = VideoClip(make_frame, duration=duration).with_fps(fps)
        video_clip = video_clip.with_audio(audio_clip)

        output_video = os.path.splitext(audio_path)[0] + ".mp4"
        video_clip.write_videofile(output_video, codec='libx264', audio_codec='aac', bitrate="2000k", threads=4, logger='bar')

        # Record final metrics
        song_name = os.path.splitext(os.path.basename(audio_path))[0]
        project_dir = rf'D:\[Library]\[Audio]\[Works]\[Projects]\{song_name}'
        if os.path.exists(project_dir):
            final_metrics = analyzer.update_metrics(num_frames)
            with open(os.path.join(project_dir, 'ratings.txt'), 'w', encoding='utf-8') as f:
                f.write(f"Rating: {final_metrics['rating']:.2f}\n")
                f.write(f"Standard Deviation: {final_metrics['std_dev']:.3f}\n")
                f.write(f"Contrast: {final_metrics['contrast']:.3f}\n")
                f.write(f"Bar Length Deviation: {final_metrics['peak_std']:.3f}\n")

        return output_video
    except Exception as e:
        traceback.print_exc()
        return None

def analyze_audio(file_path):
    """
    Analyzes raw audio data using a sliding window to mimic real-time behavior.
    """
    ensure_initialized()
    global cumulative_transience
    if cumulative_transience is None:
        raise ImportError("The 'cumulative_transience' extension module could not be loaded.")

    print(f"Analyzing {file_path}...")
    y, sr = librosa.load(file_path, sr=44100, mono=True)

    # Standard 1ms hop at 44.1kHz
    hop_samples_ms = 44
    num_frames = (len(y) + hop_samples_ms - 1) // hop_samples_ms

    times = np.arange(num_frames) * (hop_samples_ms / float(sr))

    # We need a dummy pre-analysis to get the 'full' envelopes for the video
    # But for the true ratings, we use the sliding window loop.
    full_res = cumulative_transience.analyze_audio(y, sr)

    analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=1.0, sr=sr)

    all_peaks = []

    # 100ms step loop
    step_ms = 100
    step_samples = int(sr * 0.1)

    # We follow the same logic as Max:
    # Trigger every 100ms.
    # Active zone: [T - 300ms, T - 201ms]
    # Lookahead: [T - 200ms, T]
    # Context: [T - 15.2s, T - 300ms]

    print(f"Starting sliding window analysis for {len(y)} samples...")
    num_steps = len(range(step_samples, len(y) + step_samples, step_samples))
    # Incremental optimization: We only need to push the new hop
    last_t = 0
    for t_samples in tqdm(range(step_samples, len(y) + step_samples, step_samples), total=num_steps, desc="Sliding Window Analysis", unit="step"):
        # The new 100ms hop
        hop_y = y[last_t : t_samples]
        last_t = t_samples

        active_start_samples = t_samples - step_samples - int(sr * 0.2)
        if active_start_samples < 0:
            # We still need to push the initial audio to maintain cache alignment
            analyzer.push_audio(hop_y, sr)
            continue

        window_start_samples = active_start_samples - int(sr * 15.0)
        if window_start_samples < 0: window_start_samples = 0

        buffer_start_frame = window_start_samples // hop_samples_ms
        active_start_frame = active_start_samples // hop_samples_ms

        # In the incremental model, analyze_chunk handles pushing the new hop_y
        res = analyzer.analyze_chunk(hop_y, sr, buffer_start_frame, active_start_frame)
        if res:
            all_peaks.extend(res['peaks'])
            # We take the metrics from the very last chunk for the final report
            final_metrics = res['metrics']

    print(f"Analysis loop finished. Found {len(all_peaks)} peaks. Starting pre-processing for video...")

    # Convert all_peaks to the format expected by generate_video
    peaks_list = [[] for _ in range(4)]
    for p in all_peaks:
        peaks_list[p['band_idx']].append(p['p_idx'])

    result = {
        'filename': os.path.basename(file_path),
        'times': times.tolist(),
        'sample_rate': sr,
        'max_peak_value': float(analyzer.max_peak if hasattr(analyzer, 'max_peak') else full_res['max_peak_value']),
        'onset_envs': [env.tolist() for env in full_res['onset_envs']],
        'rolling_thresholds': [rt.tolist() for rt in full_res['rolling_thresholds']],
        'peaks_list': [np.array(p, dtype=np.int32) for p in peaks_list]
    }

    # Add compatibility fields
    for i in range(4):
        result[f"onset_env_{i}"] = result['onset_envs'][i]
        result[f"rolling_threshold_{i}"] = result['rolling_thresholds'][i]
        p_indices = result['peaks_list'][i].tolist()
        result[f"peaks_{i}"] = {
            "times": [times[idx] for idx in p_indices],
            "values": [full_res['onset_envs'][i][idx] for idx in p_indices],
            "indices": p_indices
        }

    return result

def main():
    ensure_initialized()
    global cumulative_transience

    parser = argparse.ArgumentParser(description="Standalone transient analysis and video generation.")
    parser.add_argument("files", nargs="*", help="Optional list of audio files to process.")
    args = parser.parse_args()

    extensions = ('.wav', '.mp3', '.m4a', '.flac', '.ogg', '.aiff')
    audio_files = []

    # Determine search sources: provided arguments or current working directory
    sources = args.files if args.files else [os.getcwd()]

    for source in sources:
        if os.path.isdir(source):
            # Expand directory to all supported audio files within it
            dir_files = [os.path.join(source, f) for f in os.listdir(source) if f.lower().endswith(extensions)]
            audio_files.extend(dir_files)
        elif os.path.isfile(source) and source.lower().endswith(extensions):
            audio_files.append(source)

    audio_files.sort()

    if not audio_files:
        print("No audio files found to process.")

        # Help the user if they are likely running into Windows shortcut "Start in" issues
        script_dir = os.path.dirname(os.path.abspath(__file__))
        if os.getcwd().lower() == script_dir.lower():
            print("\n" + "="*60)
            print("TIP: Windows Shortcuts")
            print("="*60)
            print("If you are running this from a shortcut, Windows often sets the 'Start in'")
            print("property to the script's folder. To analyze files in the shortcut's")
            print("actual folder, please use 'analyze_here.bat' instead.")
            print("")
            print("You can copy 'analyze_here.bat' to any folder to analyze its contents,")
            print("or create a shortcut to the .bat file itself.")
            print("="*60)
        return

    for f in tqdm(audio_files, desc="Processing Audio Files", unit="file"):
        if not os.path.exists(f): continue
        result = analyze_audio(f)
        if result:
            generate_video(f, result)

if __name__ == "__main__":
    try:
        main()
        print("\nAnalysis complete.")
    except BaseException:
        print("\n" + "="*60)
        print("TERMINATION LOG")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    finally:
        # Keep window open for user to see output/errors
        print("\nPersistence check: The script will remain open until you press Enter.")
        try:
            input("\nPress Enter to exit...")
        except (EOFError, KeyboardInterrupt):
            pass

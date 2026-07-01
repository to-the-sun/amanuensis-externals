import argparse
import librosa
import numpy as np
import scipy.signal
import os
import cv2
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from moviepy import VideoClip, AudioFileClip
from rendering_engine import OCVRenderer
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

def get_score_color(score, min_score, max_score):
    """
    Returns a hex color string based on the resonance score relative to min/max seen.
    score == min_score (negative): bright red (#ff0000)
    score == 0: subdued gray (#808080)
    score == max_score (positive): bright green (#00ff00)
    Interpolates linearly in between, anchoring zero as gray.
    """
    if score == 0:
        return "#808080"

    if score < 0:
        # Interpolate between Red (#ff0000) and Gray (#808080)
        t = score / min_score if min_score < 0 else 0.0
        t = max(0, min(1, t))
        r = int(0x80 + (0xff - 0x80) * t)
        g = int(0x80 + (0x00 - 0x80) * t)
        b = int(0x80 + (0x00 - 0x80) * t)
    else:
        # Interpolate between Gray (#808080) and Green (#00ff00)
        t = score / max_score if max_score > 0 else 0.0
        t = max(0, min(1, t))
        r = int(0x80 + (0x00 - 0x80) * t)
        g = int(0x80 + (0xff - 0x80) * t)
        b = int(0x80 + (0x00 - 0x80) * t)

    return f"#{r:02x}{g:02x}{b:02x}"

def generate_video(audio_path, data):
    """
    Generates a video file for the analyzed audio showing a moving playhead over the transient graphs
    (overlapping 4-band analysis) and an accumulating 10-second buffer.
    Returns the path to the generated MP4 file.
    """
    ensure_initialized()
    if cumulative_transience is None:
        raise ImportError("The 'cumulative_transience' extension module could not be loaded.")

    print(f"Generating video for {audio_path}...")
    try:
        times = data['times']
        onset_envs = data['onset_envs']
        rolling_thresholds = data['rolling_thresholds']
        peak_indices_list = data['peaks_list']
        max_peak = data['max_peak_value']
        all_valid_peak_indices = set().union(*peak_indices_list)

        duration = times[-1]
        fps = 30
        num_video_frames = int(duration * fps)
        video_frame_times = np.arange(num_video_frames) / float(fps)
        frame_indices = np.searchsorted(np.array(times), video_frame_times)
        num_frames = len(frame_indices)

        # 1. Generate Static Background using Matplotlib
        fig, (ax_transient, ax_snapshot, ax_buf) = plt.subplots(3, 1, figsize=(12, 14), gridspec_kw={'height_ratios': [1, 0.4, 1]})
        ax_transient.set_title(f"4-Band Transient Analysis - {os.path.basename(audio_path)}")
        ax_transient.set_ylabel("Onset Strength")
        ax_transient.grid(True, alpha=0.3)
        ax_transient.set_xlim(-20, 5) # Placeholder
        def format_time(x, pos):
            m = int(abs(x) // 60)
            s = int(abs(x) % 60)
            prefix = "-" if x < 0 else ""
            return f"{prefix}{m}:{s:02d}"
        ax_transient.xaxis.set_major_formatter(ticker.FuncFormatter(format_time))
        all_onset_vals = np.concatenate(onset_envs)
        ax_transient.set_ylim(0, max(all_onset_vals) * 1.1 if len(all_onset_vals) > 0 else 1)

        ax_snapshot.set_xlim(-45, 1)
        ax_snapshot.set_ylim(-0.5, 3.5)
        ax_snapshot.set_yticks([0, 1, 2, 3])
        ax_snapshot.set_yticklabels(['Sub', 'Bass', 'Mid', 'Hi'], fontsize=10, fontweight='bold')
        ax_snapshot.set_title("39ms Rolling Window Snapshot", fontsize=14, fontweight='bold')
        ax_snapshot.set_xlabel("Time Relative to Latest Peak (ms)", fontsize=12)

        ax_buf.set_title("Accumulated 5s Historical Buffer")
        ax_buf.set_xlabel("Time Relative to Peak (ms)")
        ax_buf.set_ylabel("Accumulated Energy")
        ax_buf.grid(True, alpha=0.3)
        ax_buf.set_xlim(-5000, 0)
        ax_buf.set_ylim(0, 1)

        fig.tight_layout(pad=1.5)
        fig.canvas.draw()
        # Handle different matplotlib versions
        if hasattr(fig.canvas, 'tostring_rgb'):
            bg_rgb = np.frombuffer(fig.canvas.tostring_rgb(), dtype=np.uint8)
        else:
            bg_rgb = np.frombuffer(fig.canvas.buffer_rgba(), dtype=np.uint8)
            # RGBA to RGB
            bg_rgb = bg_rgb.reshape(-1, 4)[:, :3].flatten()

        bg_rgb = bg_rgb.reshape(fig.canvas.get_width_height()[::-1] + (3,))

        # Capture metadata for renderer
        layout_metadata = {}
        for key, ax in zip(['ax_transient', 'ax_snapshot', 'ax_buf'], [ax_transient, ax_snapshot, ax_buf]):
            bbox = ax.get_position()
            width, height = fig.get_size_inches() * fig.dpi
            layout_metadata[key] = {
                'bbox_px': [bbox.x0 * width, (1-bbox.y1) * height, bbox.x1 * width, (1-bbox.y0) * height],
                'xlim': ax.get_xlim(),
                'ylim': ax.get_ylim()
            }
        plt.close(fig)

        # 2. Initialize OCV Renderer
        renderer = OCVRenderer(bg_rgb, layout_metadata)
        static_data = {
            'title': f"4-Band Transient Analysis - {os.path.basename(audio_path)}",
            'times': times,
            'max_onset_val': max(np.concatenate(onset_envs)) if any(len(e) > 0 for e in onset_envs) else 1.0,
            'onset_envs': onset_envs,
            'rolling_thresholds': rolling_thresholds
        }

        POPUP_LIFETIME = 60
        analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=max_peak, sr=data.get('sample_rate', 44100))
        peaks_params_list = data.get('peaks_params_list', None)

        # State for dynamic elements
        state = {
            'live_peaks': [], # list of (time, val)
            'rolling_window_scores': [],
            'active_flashes': [],
            'active_scores': [],
            'active_qualifiers': [],
            'active_debug_lines': [],
            'current_snapshot_avg': 0.0,
            'min_score': 0.0,
            'max_score': 0.0,
            'rating': 0.0,
            'metrics': {'std_dev': 0.0, 'contrast': 0.0, 'peak_std': 0.0},
            'accumulated_buffer': np.zeros(5001),
            'mean': 0.0,
            'highest_peak_ms': None
        }

        last_computed_frame = -1
        def make_frame(t):
            nonlocal last_computed_frame
            frame_idx = int(t * fps)
            if frame_idx >= len(frame_indices): frame_idx = len(frame_indices) - 1

            # Ensure sequential processing for stateful elements
            for idx in range(last_computed_frame + 1, frame_idx + 1):
                frame = frame_indices[idx]

                # 1. Update Analyzer State
                live_peak_results = analyzer.process_new_peaks(frame, peak_indices_list, onset_envs, all_valid_peak_indices, times, peaks_params_list)
                metrics = analyzer.update_metrics(frame)

                state['min_score'] = metrics['min_score_seen']
                state['max_score'] = metrics['max_score_seen']
                state['rating'] = metrics['rating']
                state['metrics'] = metrics
                state['accumulated_buffer'] = analyzer.accumulated_buffer
                state['mean'] = metrics['mean']
                state['highest_peak_ms'] = metrics['highest_peak_ms']

                for p in live_peak_results:
                    state['rolling_window_scores'].append({'frame': p['p_idx'], 'score': p['total_score'], 'band_idx': p['band_idx']})
                    state['live_peaks'].append((p['time'], p['peak_val']))

                    q_sum = sum(q['val'] for q in p['qualifiers'])
                    f_val = p.get('detected_peak_val', p['peak_val'])
                    debug_msg = (f"[B{p['band_idx']}] (Flux:{f_val:.2f} > Th:{p['thresh_val']:.2f} & "
                                 f"Flux >= 3.0 & Pr:{p['prominence']:.2f} >= 0.50) | "
                                 f"Score:{p['total_score']:+.2f} = {p['peak_val']:.2f} * {q_sum:.2f}")
                    state['active_debug_lines'].insert(0, {'text': debug_msg, 'lifetime': POPUP_LIFETIME, 'band_idx': p['band_idx']})

                    if p['snapshot'] is not None:
                        state['active_qualifiers'].clear()
                        for q_info in p['qualifiers']:
                            state['active_qualifiers'].append({'lifetime': POPUP_LIFETIME, 'ms': q_info['ms'], 'val': q_info['val']})

                        state['active_scores'].append({'lifetime': POPUP_LIFETIME, 'y': p['peak_val'], 'val': p['total_score'], 'time': p['time']})
                        state['active_flashes'].append({'lifetime': POPUP_LIFETIME, 'snapshot': p['snapshot'].copy()})

                # 2. Prune and Update lifetimes
                state['rolling_window_scores'] = [s for s in state['rolling_window_scores'] if s['frame'] > frame - 39]
                if state['rolling_window_scores']:
                    state['current_snapshot_avg'] = sum(s['score'] for s in state['rolling_window_scores']) / len(state['rolling_window_scores'])

                for flash in state['active_flashes'][:]:
                    flash['lifetime'] -= 1
                    if flash['lifetime'] <= 0: state['active_flashes'].remove(flash)

                for score in state['active_scores'][:]:
                    score['lifetime'] -= 1
                    if score['lifetime'] <= 0: state['active_scores'].remove(score)

                for q in state['active_qualifiers'][:]:
                    q['lifetime'] -= 1
                    if q['lifetime'] <= 0: state['active_qualifiers'].remove(q)

                for d in state['active_debug_lines'][:]:
                    d['lifetime'] -= 1
                    if d['lifetime'] <= 0: state['active_debug_lines'].remove(d)

            last_computed_frame = frame_idx

            # 3. Call Renderer
            render_frame_data = {
                'frame': frame_indices[frame_idx],
                'live_peaks': state['live_peaks'],
                'rolling_window_scores': state['rolling_window_scores'],
                'min_score': state['min_score'],
                'max_score': state['max_score'],
                'accumulated_buffer': state['accumulated_buffer'],
                'mean': state['mean'],
                'active_flashes': [{'alpha': (f['lifetime']/POPUP_LIFETIME)*0.5, 'snapshot': f['snapshot']} for f in state['active_flashes']],
                'highest_peak_ms': state['highest_peak_ms'],
                'current_score': state['current_snapshot_avg'],
                'rating': state['rating'],
                'metrics': state['metrics'],
                'debug_lines': [{'text': d['text'], 'band_idx': d['band_idx'], 'alpha': min(1.0, d['lifetime']/10.0)} for d in state['active_debug_lines'][:10]],
                'active_scores': [{'val': s['val'], 'time': s['time'], 'y': s['y'] + (1 - s['lifetime']/POPUP_LIFETIME)*0.1*static_data['max_onset_val'], 'alpha': s['lifetime']/POPUP_LIFETIME} for s in state['active_scores']],
                'active_qualifiers': [{'ms': q['ms'], 'val': q['val'], 'alpha': q['lifetime']/POPUP_LIFETIME} for q in state['active_qualifiers']]
            }

            return renderer.render_frame(render_frame_data, static_data)

        # Mux audio and render in one pass
        output_video = os.path.splitext(audio_path)[0] + ".mp4"
        print(f"Saving video to: {output_video}")
        try:
            audio_clip = AudioFileClip(audio_path)
            clip = VideoClip(make_frame, duration=duration).with_audio(audio_clip)
            clip.write_videofile(output_video, fps=fps, codec='libx264', audio_codec='aac',
                                 logger='bar' if sys.stdout.isatty() else None)
        except Exception as e:
            print(f"Error generating video with audio: {e}")
            traceback.print_exc()
            # Fallback to silent video
            clip = VideoClip(make_frame, duration=duration)
            clip.write_videofile(output_video, fps=fps, codec='libx264', logger='bar' if sys.stdout.isatty() else None)

        # Record final metrics
        try:
            final_metrics = analyzer.update_metrics(len(times)-1)
            # Re-calculate batch-final average based on true final 39ms window if desired,
            # but usually ratings.txt uses the global average 'rating' from C.
            song_name = os.path.splitext(os.path.basename(audio_path))[0]
            project_dir = rf'D:\[Library]\[Audio]\[Works]\[Projects]\{song_name}'
            if os.path.exists(project_dir):
                ratings_file = os.path.join(project_dir, 'ratings.txt')
                with open(ratings_file, 'w', encoding='utf-8') as f:
                    f.write(f"Rating: {final_metrics['rating']:.2f}\n")
                    f.write(f"Standard Deviation: {final_metrics['std_dev']:.3f}\n")
                    f.write(f"Contrast: {final_metrics['contrast']:.3f}\n")
                    f.write(f"Bar Length Deviation: {final_metrics['peak_std']:.3f}\n")
                print(f"Metrics recorded to {ratings_file}")
            else:
                print(f"Skipping recording metrics: {project_dir} does not exist.")
        except Exception as e:
            print(f"Error recording metrics: {e}")

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
    # and capture the detailed parameters for synchronization
    peaks_list = [[] for _ in range(4)]
    peaks_params_list = [{'thresh_vals': [], 'left_mins': [], 'right_mins': [], 'proms': [], 'detected_peak_vals': []} for _ in range(4)]

    for p in all_peaks:
        b = p['band_idx']
        peaks_list[b].append(p['p_idx'])
        params = peaks_params_list[b]
        params['thresh_vals'].append(p.get('thresh_val', 0.0))
        params['left_mins'].append(p.get('left_min', 0.0))
        params['right_mins'].append(p.get('right_min', 0.0))
        params['proms'].append(p.get('prominence', 0.0))
        params['detected_peak_vals'].append(p.get('detected_peak_val', 0.0))

    result = {
        'filename': os.path.basename(file_path),
        'times': times.tolist(),
        'sample_rate': sr,
        'max_peak_value': float(analyzer.max_peak if hasattr(analyzer, 'max_peak') else full_res['max_peak_value']),
        'onset_envs': [env.tolist() for env in full_res['onset_envs']],
        'rolling_thresholds': [rt.tolist() for rt in full_res['rolling_thresholds']],
        'peaks_list': [np.array(p, dtype=np.int32) for p in peaks_list],
        'peaks_params_list': peaks_params_list
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

import argparse
import librosa
import numpy as np
import scipy.signal
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import matplotlib.ticker as ticker
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
        
        # Helper for secondary peak lookup
        all_valid_peak_indices = set().union(*peak_indices_list)

        fig, (ax_transient, ax_snapshot, ax_buf) = plt.subplots(3, 1, figsize=(12, 14), 
                                                               gridspec_kw={'height_ratios': [1, 0.4, 1]})

        colors = ['#1b4f72', '#3498db', '#2ecc71', '#a9dfbf']
        alphas = [1.0, 0.8, 0.6, 0.4]
        labels = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble']

        transient_lines = []
        threshold_lines = []
        for i in range(4):
            line, = ax_transient.plot(times, onset_envs[i], color=colors[i], lw=2, alpha=alphas[i], label=labels[i])
            transient_lines.append(line)
            t_line, = ax_transient.plot([times[0], times[-1]], [0, 0], color=colors[i], lw=1, ls='--', alpha=0.5)
            threshold_lines.append(t_line)
            # Add scatter for peaks
            p_indices = list(peak_indices_list[i])
            peak_times = [times[idx] for idx in p_indices]
            peak_vals = [onset_envs[i][idx] for idx in p_indices]
            ax_transient.scatter(peak_times, peak_vals, color='#e74c3c', marker='x', s=30, alpha=alphas[i])

        playhead_transient = ax_transient.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead')
        cleanup_transient = ax_transient.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')

        ax_transient.set_title(f"4-Band Transient Analysis - {os.path.basename(audio_path)}")
        ax_transient.set_ylabel("Onset Strength")
        ax_transient.legend(loc='upper right')
        ax_transient.grid(True, alpha=0.3)
        ax_transient.set_xlim(-20, 5)

        def format_time(x, pos):
            m = int(abs(x) // 60)
            s = int(abs(x) % 60)
            prefix = "-" if x < 0 else ""
            return f"{prefix}{m}:{s:02d}"
        ax_transient.xaxis.set_major_formatter(ticker.FuncFormatter(format_time))

        all_onset_vals = np.concatenate(onset_envs)
        ax_transient.set_ylim(0, max(all_onset_vals) * 1.1 if len(all_onset_vals) > 0 else 1)

        buffer_times = np.linspace(-5000, 0, 5001)
        buffer_line, = ax_buf.plot(buffer_times, np.zeros(5001), color='#f1c40f', lw=2)
        mean_line, = ax_buf.plot([-5000, 0], [0, 0], color='#808080', lw=1, ls='--', alpha=0.5, label='Mean Energy')
        ax_buf.set_title("Accumulated 5s Historical Buffer")
        ax_buf.set_xlabel("Time Relative to Peak (ms)")
        ax_buf.set_ylabel("Accumulated Energy")
        ax_buf.grid(True, alpha=0.3)
        ax_buf.set_xlim(-5000, 0)
        ax_buf.set_ylim(0, 1)

        # Configure Snapshot bar
        ax_snapshot.set_xlim(-39, 1) # Extra space for labels
        ax_snapshot.set_ylim(-0.5, 3.5) # 4 lanes: 0, 1, 2, 3
        ax_snapshot.set_yticks([0, 1, 2, 3])
        ax_snapshot.set_yticklabels(['Sub', 'Bass', 'Mid', 'Hi'], fontsize=10, fontweight='bold')
        ax_snapshot.set_title("39ms Rolling Window Snapshot", fontsize=14, fontweight='bold')
        ax_snapshot.set_xlabel("Time Relative to Playhead (ms)", fontsize=12)
        ax_snapshot.grid(False)
        for i in range(3):
            ax_snapshot.axhline(i + 0.5, color='gray', lw=1, alpha=0.3)

        active_flashes = []
        flash_fill_artists = []
        peak_lines = []
        active_scores = [] # List of [text_artist, lifetime, initial_y, val]
        active_qualifiers = [] # List of [line, label, lifetime, val]
        snapshot_artists = [] # List of hash marks and labels
        last_frame_processed = -1
        current_snapshot_avg = 0.0
        rolling_window_scores = [] # List of {'frame', 'score', 'band_idx'}

        score_display_text = ax_transient.text(0.02, 0.98, 'Score: +0.00', transform=ax_transient.transAxes,
                                              verticalalignment='top', fontsize=20, color='#808080',
                                              fontweight='bold')

        rating_text = ax_transient.text(0.02, 0.90, 'Rating: 0.00', transform=ax_transient.transAxes,
                                        verticalalignment='top', fontsize=12, color='#f1c40f',
                                        fontweight='bold')

        metrics_text = ax_buf.text(0.02, 0.95, '', transform=ax_buf.transAxes,
                                   verticalalignment='top', fontsize=10, color='#f1c40f',
                                   fontweight='bold')

        # Instantiate analyzer
        analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=max_peak)

        def update(frame):
            nonlocal last_frame_processed, current_snapshot_avg, rolling_window_scores
            
            # Catch up C core and accumulate scores in Python
            all_new_peak_data = []
            for f in range(last_frame_processed + 1, frame + 1):
                new_peak_data = analyzer.process_new_peaks(f, peak_indices_list, onset_envs, all_valid_peak_indices, times)
                for p in new_peak_data:
                    rolling_window_scores.append({
                        'frame': p['p_idx'],
                        'score': p['total_score'],
                        'band_idx': p['band_idx']
                    })
                all_new_peak_data.extend(new_peak_data)
                analyzer.update_metrics(f)
            
            last_frame_processed = frame

            # Prune scores that have left the 39ms window
            # Window is [frame - 39, frame]
            prev_count = len(rolling_window_scores)
            rolling_window_scores = [s for s in rolling_window_scores if s['frame'] > frame - 39]
            
            # Update visualization if set changed or new peaks arrived
            if len(rolling_window_scores) != prev_count or all_new_peak_data:
                if rolling_window_scores:
                    for artist in snapshot_artists:
                        artist.remove()
                    snapshot_artists.clear()

                    current_snapshot_avg = sum(s['score'] for s in rolling_window_scores) / len(rolling_window_scores)
                    
                    for s in rolling_window_scores:
                        rel_ms = float(s['frame'] - frame)
                        score_val = s['score']
                        band_idx = s['band_idx']
                        band_c = colors[band_idx]
                        lane_y = band_idx 

                        line = ax_snapshot.vlines(x=rel_ms, ymin=lane_y - 0.4, ymax=lane_y + 0.4, 
                                                 color=band_c, lw=3)
                        txt = ax_snapshot.text(rel_ms + 0.5, lane_y, f"{score_val:+.2f}", 
                                               color=band_c, fontsize=13, va='center', fontweight='bold')
                        snapshot_artists.extend([line, txt])

            current_time = times[frame]
            ax_transient.set_xlim(current_time - 20, current_time + 5)

            for i in range(4):
                threshold_lines[i].set_ydata([rolling_thresholds[i][frame], rolling_thresholds[i][frame]])
                threshold_lines[i].set_xdata([current_time - 20, current_time + 5])

            playhead_transient.set_xdata([current_time, current_time])
            cleanup_time = current_time - 15
            cleanup_transient.set_xdata([cleanup_time, cleanup_time])

            # Process visually appearing Peaks
            for p_data in all_new_peak_data:
                # Clear existing qualifiers when a new peak is processed
                for q in active_qualifiers:
                    q[0].remove()
                    q[1].remove()
                active_qualifiers.clear()

                if p_data['snapshot'] is not None:
                    # Create individual qualifier markers
                    for q_info in p_data['qualifiers']:
                        q_ms = q_info['ms']
                        qualifier = q_info['val']
                        q_color = get_score_color(qualifier, -1.0, 1.0)
                        q_line = ax_buf.axvline(x=q_ms, color=q_color, lw=3.0, ls=':', alpha=0.8)
                        q_label = ax_buf.text(q_ms, (qualifier + 1) / 2, f"{qualifier:+.2f}",
                                              color=q_color, fontsize=8, ha='left', va='center',
                                              transform=ax_buf.get_xaxis_transform())
                        active_qualifiers.append([q_line, q_label, 20, qualifier])

                    # Update previous scores to be smaller
                    for score in active_scores:
                        score[0].set_fontsize(10)
                        score[0].set_fontweight('normal')

                    # Create score animation
                    total_score = p_data['total_score']
                    score_text = ax_transient.text(p_data['time'], p_data['peak_val'], f"{total_score:+.2f}",
                                                color=get_score_color(total_score, analyzer.min_score_seen, analyzer.max_score_seen),
                                                fontsize=20, fontweight='bold',
                                                ha='center', va='bottom')
                    active_scores.append([score_text, 20, p_data['peak_val'], total_score])
                    active_flashes.append([p_data['snapshot'], 20])

            # Update Metrics and Cleanup
            metrics = analyzer.update_metrics(frame)
            
            if metrics['buffer_updated'] or all_new_peak_data:
                buffer_line.set_ydata(analyzer.accumulated_buffer)
            
            # Dynamic Y-axis scaling for buffer
            current_max = np.max(analyzer.accumulated_buffer[:-99]) if len(analyzer.accumulated_buffer) > 99 else 0
            ax_buf.set_ylim(0, max(0.1, current_max * 1.1))

            mean_line.set_ydata([metrics['mean'], metrics['mean']])
            metrics_text.set_text(f"Std Dev: {metrics['std_dev']:.3f}\nContrast: {metrics['contrast']:.3f}\nPeak Std: {metrics['peak_std']:.3f}")
            rating_text.set_text(f"Rating: {metrics['rating']:.2f}")
            score_display_text.set_text(f"Score: {current_snapshot_avg:+.2f}")
            score_display_text.set_color(get_score_color(current_snapshot_avg, metrics['min_score_seen'], metrics['max_score_seen']))

            # Handle Flash and Fade
            for artist in flash_fill_artists:
                artist.remove()
            flash_fill_artists.clear()

            for flash in active_flashes[:]:
                snapshot, lifetime = flash
                alpha = (lifetime / 20.0) * 0.5
                if np.any(snapshot > 0):
                    fill = ax_buf.fill_between(buffer_times, 0, snapshot, color='#2ecc71', alpha=alpha)
                    flash_fill_artists.append(fill)
                flash[1] -= 1
                if flash[1] <= 0:
                    active_flashes.remove(flash)

            # Handle Score Animations
            current_ylim = ax_transient.get_ylim()[1]
            for score in active_scores[:]:
                txt, lifetime, initial_y, val = score
                lifetime -= 1
                if lifetime <= 0:
                    txt.remove()
                    active_scores.remove(score)
                else:
                    score[1] = lifetime
                    progress = (20 - lifetime) / 20.0
                    new_y = initial_y + (progress * 0.1 * current_ylim)
                    txt.set_position((txt.get_position()[0], new_y))
                    txt.set_alpha(lifetime / 20.0)
                    txt.set_color(get_score_color(val, analyzer.min_score_seen, analyzer.max_score_seen))

            # Handle highest peak line
            for line in peak_lines:
                line.remove()
            peak_lines.clear()
            if metrics['highest_peak_ms'] is not None:
                line = ax_buf.axvline(x=metrics['highest_peak_ms'], color='#f1c40f', lw=2, ls='--')
                peak_lines.append(line)

            # Handle Qualifier Animations
            for q in active_qualifiers[:]:
                q_line, q_label, lifetime, val = q
                lifetime -= 1
                if lifetime <= 0:
                    q_line.remove()
                    q_label.remove()
                    active_qualifiers.remove(q)
                else:
                    q[2] = lifetime
                    alpha = lifetime / 20.0
                    q_line.set_alpha(alpha * 0.8)
                    q_label.set_alpha(alpha)

            score_artists = [s[0] for s in active_scores]
            qualifier_artists = []
            for q in active_qualifiers:
                qualifier_artists.append(q[0])
                qualifier_artists.append(q[1])

            return [playhead_transient, cleanup_transient, buffer_line, mean_line, metrics_text, rating_text, score_display_text] + threshold_lines + flash_fill_artists + peak_lines + score_artists + qualifier_artists + snapshot_artists

        frame_indices = range(0, len(times), 33)
        num_frames = len(frame_indices)
        ani = animation.FuncAnimation(fig, update, frames=frame_indices, blit=False, interval=33)

        with tempfile.NamedTemporaryFile(suffix=".mp4", delete=False) as tmp:
            temp_video_path = tmp.name

        pbar = tqdm(total=num_frames, desc="Rendering Video", unit="frame")
        def progress_callback(i, n):
            pbar.n = i + 1
            pbar.refresh()

        writer = animation.FFMpegWriter(fps=30, metadata=dict(artist='Transient Analysis Tool'), bitrate=2000)
        fig.tight_layout(pad=1.5)
        ani.save(temp_video_path, writer=writer, progress_callback=progress_callback)
        pbar.close()
        plt.close(fig)

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

        output_video = os.path.splitext(audio_path)[0] + ".mp4"
        if shutil.which("ffmpeg"):
            cmd = ['ffmpeg', '-y', '-i', temp_video_path, '-i', audio_path, '-c:v', 'copy', '-c:a', 'aac', '-ac', '1', '-shortest', output_video]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if os.path.exists(temp_video_path): os.remove(temp_video_path)
            print(f"Video generated: {output_video}")
            return output_video
        else:
            print("Error: ffmpeg not found.")
            return None
    except Exception as e:
        print(f"Error generating video: {e}")
        return None

def analyze_audio(file_path):
    """
    Analyzes raw audio data to extract its transient envelope (4-band analysis)
    and identify peaks. Returns a dictionary with all analysis data.
    """
    ensure_initialized()
    global cumulative_transience
    if cumulative_transience is None:
        raise ImportError("The 'cumulative_transience' extension module could not be loaded.")

    print(f"Analyzing {file_path}...")
    y, sr = librosa.load(file_path, sr=None, mono=True)
    result = cumulative_transience.analyze_audio(y, sr)
    result['filename'] = os.path.basename(file_path)
    
    # Compatibility with existing result format
    result['times'] = result['times'].tolist()
    for i in range(4):
        result[f"onset_env_{i}"] = result['onset_envs'][i].tolist()
        result[f"rolling_threshold_{i}"] = result['rolling_thresholds'][i].tolist()
        result[f"peaks_{i}"] = {
            "times": result['times'][result['peaks_list'][i]].tolist() if isinstance(result['times'], np.ndarray) else [result['times'][idx] for idx in result['peaks_list'][i]],
            "values": result['onset_envs'][i][result['peaks_list'][i]].tolist(),
            "indices": result['peaks_list'][i].tolist()
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

    for f in audio_files:
        if not os.path.exists(f): continue
        result = analyze_audio(f)
        if result:
            generate_video(f, result)

if __name__ == "__main__":
    try:
        main()
        print("\nAnalysis complete.")
    except Exception as e:
        print("\n" + "="*60)
        print("CRITICAL ERROR")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    finally:
        # Keep window open for user to see output/errors
        try:
            input("\nPress Enter to exit...")
        except EOFError:
            pass

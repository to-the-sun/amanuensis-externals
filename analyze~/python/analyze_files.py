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
        # ax_transient.legend(loc='upper right') # Legend removed per user request
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
        ax_snapshot.set_xlim(-45, 1) # Extra space for labels
        ax_snapshot.set_ylim(-0.5, 3.5) # 4 lanes: 0, 1, 2, 3
        ax_snapshot.set_yticks([0, 1, 2, 3])
        ax_snapshot.set_yticklabels(['Sub', 'Bass', 'Mid', 'Hi'], fontsize=10, fontweight='bold')
        ax_snapshot.set_title("39ms Rolling Window Snapshot", fontsize=14, fontweight='bold')
        ax_snapshot.set_xlabel("Time Relative to Latest Peak (ms)", fontsize=12)
        ax_snapshot.grid(False)
        for i in range(3):
            ax_snapshot.axhline(i + 0.5, color='gray', lw=1, alpha=0.3)

        POPUP_LIFETIME = 60
        MAX_POOL = 128

        # Strategy 3: Pre-calculate X indices for buffer
        snap_verts_x = np.concatenate([[buffer_times[0]], buffer_times, [buffer_times[-1]]])

        # Artist Pools
        pool_scores = [ax_transient.text(0, 0, '', visible=False) for _ in range(MAX_POOL)]
        pool_qualifier_lines = [ax_buf.axvline(0, visible=False, lw=3.0, ls=':', alpha=0.8) for _ in range(MAX_POOL)]
        pool_qualifier_labels = [ax_buf.text(0, 0, '', visible=False, fontsize=8, transform=ax_buf.get_xaxis_transform()) for _ in range(MAX_POOL)]
        pool_snapshots = [ax_buf.fill_between(buffer_times, 0, 0, visible=False, color='#2ecc71') for _ in range(MAX_POOL)]

        # Snapshot artists pool (4 lanes * max peaks in 39ms)
        MAX_SNAPSHOT_POOL = 32
        # LineCollection is more efficient than individual vlines
        from matplotlib.collections import LineCollection
        pool_snap_lines = LineCollection([], colors=[], linewidths=3, visible=False)
        ax_snapshot.add_collection(pool_snap_lines)
        pool_snap_texts = [ax_snapshot.text(0, 0, '', visible=False, fontsize=13, va='center', ha='right', fontweight='bold') for _ in range(MAX_SNAPSHOT_POOL)]

        active_flashes = [] # [pool_idx, lifetime]
        active_scores = []  # [pool_idx, lifetime, initial_y, val, time]
        active_qualifiers = [] # [pool_idx, lifetime, ms, val]

        # Pool for 'highest peak' line
        highest_peak_line = ax_buf.axvline(0, visible=False, color='#f1c40f', lw=2, ls='--')
        last_frame_processed = -1
        current_snapshot_avg = 0.0
        rolling_window_scores = [] # List of {'frame', 'score', 'band_idx'}

        score_display_text = ax_transient.text(0.02, 0.98, 'Score: +0.00', transform=ax_transient.transAxes,
                                              verticalalignment='top', fontsize=20, color='#808080',
                                              fontweight='bold', zorder=10)

        rating_text = ax_transient.text(0.02, 0.90, 'Rating: 0.00', transform=ax_transient.transAxes,
                                        verticalalignment='top', fontsize=12, color='#f1c40f',
                                        fontweight='bold', zorder=10)

        # Debug console pool
        MAX_DEBUG_LINES = 10
        debug_console_pool = [ax_transient.text(0.98, 0.98 - (i * 0.05), '', transform=ax_transient.transAxes,
                                               verticalalignment='top', horizontalalignment='right',
                                               fontsize=11, family='monospace', color='#2ecc71',
                                               fontweight='bold', visible=False, zorder=10)
                             for i in range(MAX_DEBUG_LINES)]
        active_debug_lines = [] # list of {'text', 'lifetime', 'band_idx'}

        metrics_text = ax_buf.text(0.02, 0.95, '', transform=ax_buf.transAxes,
                                   verticalalignment='top', fontsize=10, color='#f1c40f',
                                   fontweight='bold')

        # Instantiate analyzer
        analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=max_peak, sr=data.get('sample_rate', 44100))

        # Pre-process all peaks for the entire file to avoid re-running the heavy sliding window
        # during animation. This list will be consumed by the update() function.
        all_processed_peaks = []
        peaks_params_list = data.get('peaks_params_list', None)
        for p_idx in tqdm(sorted(all_valid_peak_indices), desc="Pre-processing Peaks", unit="peak"):
             # Finding which band this peak belongs to
             band_idx = -1
             for b in range(4):
                 if p_idx in peak_indices_list[b]:
                     band_idx = b
                     break

             # Process it
             res_list = analyzer.process_new_peaks(p_idx, peak_indices_list, onset_envs, all_valid_peak_indices, times, peaks_params_list)
             all_processed_peaks.extend(res_list)

        def update(frame):
            nonlocal last_frame_processed, current_snapshot_avg, rolling_window_scores, active_debug_lines

            # Reset visibility of all pooled artists for blitting safety
            # (Though technically only the ones that were active last frame need reset)
            for s in pool_scores: s.set_visible(False)
            for l in pool_qualifier_lines: l.set_visible(False)
            for t in pool_qualifier_labels: t.set_visible(False)
            for f in pool_snapshots: f.set_visible(False)
            for d in debug_console_pool: d.set_visible(False)
            pool_snap_lines.set_visible(False)
            for st in pool_snap_texts: st.set_visible(False)
            highest_peak_line.set_visible(False)

            # Use pre-processed peaks for this visual frame
            all_new_peak_data = [p for p in all_processed_peaks if p['p_idx'] > last_frame_processed and p['p_idx'] <= frame]

            for p in all_new_peak_data:
                rolling_window_scores.append({
                    'frame': p['p_idx'],
                    'score': p['total_score'],
                    'band_idx': p['band_idx']
                })

            analyzer.update_metrics(frame)
            last_frame_processed = frame

            # Prune scores to the 39ms sliding window ending at the current playhead frame
            rolling_window_scores = [s for s in rolling_window_scores if s['frame'] > frame - 39]

            # Calculate rolling average and update visualization only if we have scores
            # This ensures the snapshot bar and score stay fixed when the window is empty
            if rolling_window_scores:
                current_snapshot_avg = sum(s['score'] for s in rolling_window_scores) / len(rolling_window_scores)

                # Align relative to the LATEST peak in the current 39ms window
                # This ensures the most current score is always at the far right (x=0)
                latest_p_frame = max(s['frame'] for s in rolling_window_scores)

                segments = []
                seg_colors = []

                for i, s in enumerate(rolling_window_scores[:MAX_SNAPSHOT_POOL]):
                    rel_ms = float(s['frame'] - latest_p_frame)
                    score_val = s['score']
                    band_idx = s['band_idx']
                    lane_y = band_idx

                    score_c = get_score_color(score_val, analyzer.min_score_seen, analyzer.max_score_seen)

                    segments.append([[rel_ms, lane_y - 0.4], [rel_ms, lane_y + 0.4]])
                    seg_colors.append(colors[band_idx])

                    txt = pool_snap_texts[i]
                    txt.set_position((rel_ms - 0.8, lane_y))
                    txt.set_text(f"{score_val:+.2f}")
                    txt.set_color(score_c)
                    txt.set_visible(True)

                if segments:
                    pool_snap_lines.set_segments(segments)
                    pool_snap_lines.set_colors(seg_colors)
                    pool_snap_lines.set_visible(True)

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
                # Add to debug console
                # Full resonance equation: Flux: {peak} > Thresh: {thresh} & Prom: {prom} >= 0.5 | Score: {score} = Flux * ΣQual
                q_sum = sum(q['val'] for q in p_data['qualifiers'])
                # Qualification Equation: (Flux > Thresh & Flux >= 1.0 & Prom >= 0.5) -> Score = Flux * sum(Quals)
                debug_msg = (f"[B{p_data['band_idx']}] (Flux:{p_data['peak_val']:.2f} > Th:{p_data['thresh_val']:.2f} & "
                             f"Flux >= 1.0 & Pr:{p_data['prominence']:.2f} >= 0.50) | "
                             f"Score:{p_data['total_score']:+.2f} = {p_data['peak_val']:.2f} * {q_sum:.2f}")

                active_debug_lines.insert(0, {'text': debug_msg, 'lifetime': POPUP_LIFETIME, 'band_idx': p_data['band_idx']})
                if len(active_debug_lines) > MAX_DEBUG_LINES:
                    active_debug_lines = active_debug_lines[:MAX_DEBUG_LINES]

                # Use pools instead of creating artists
                if p_data['snapshot'] is not None:
                    # Clear existing qualifiers by ending their lifetime or just clearing the list
                    # Matplotlib blitting requires we keep the list consistent
                    active_qualifiers.clear()

                    for q_info in p_data['qualifiers']:
                        if len(active_qualifiers) < MAX_POOL:
                            idx = len(active_qualifiers)
                            active_qualifiers.append([idx, POPUP_LIFETIME, q_info['ms'], q_info['val']])

                    # Score animation pool
                    for i in range(MAX_POOL):
                        if not any(a[0] == i for a in active_scores):
                            active_scores.append([i, POPUP_LIFETIME, p_data['peak_val'], p_data['total_score'], p_data['time']])
                            break

                    # Flash pool
                    for i in range(MAX_POOL):
                        if not any(a[0] == i for a in active_flashes):
                            # Update PolyCollection path
                            snap = p_data['snapshot']
                            poly = pool_snapshots[i]
                            # Pre-calculate points for fill_between replacement (Strategy 3)
                            verts = np.zeros((len(snap_verts_x), 2))
                            verts[:, 0] = snap_verts_x
                            verts[1:-1, 1] = snap
                            poly.set_paths([verts])

                            active_flashes.append([i, POPUP_LIFETIME])
                            break

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

            # Handle Flash and Fade (Object Pool)
            for flash in active_flashes[:]:
                idx, life = flash
                poly = pool_snapshots[idx]
                if life > 0:
                    alpha = (life / float(POPUP_LIFETIME)) * 0.5
                    poly.set_alpha(alpha)
                    poly.set_visible(True)
                    flash[1] -= 1
                if flash[1] <= 0:
                    active_flashes.remove(flash)

            # Handle Score Animations (Object Pool)
            current_ylim = ax_transient.get_ylim()[1]
            for score in active_scores[:]:
                idx, life, initial_y, val, p_time = score
                txt = pool_scores[idx]
                if life > 0:
                    progress = (POPUP_LIFETIME - life) / float(POPUP_LIFETIME)
                    new_y = initial_y + (progress * 0.1 * current_ylim)
                    txt.set_position((p_time, new_y))
                    txt.set_text(f"{val:+.2f}")
                    txt.set_alpha(life / float(POPUP_LIFETIME))
                    txt.set_color(get_score_color(val, analyzer.min_score_seen, analyzer.max_score_seen))
                    txt.set_visible(True)
                    score[1] -= 1
                if score[1] <= 0:
                    active_scores.remove(score)

            # Handle highest peak line (Object Pool)
            if metrics['highest_peak_ms'] is not None:
                highest_peak_line.set_xdata([metrics['highest_peak_ms'], metrics['highest_peak_ms']])
                highest_peak_line.set_visible(True)

            # Handle Qualifier Animations (Object Pool)
            for q in active_qualifiers[:]:
                idx, life, ms, val = q
                line = pool_qualifier_lines[idx]
                label = pool_qualifier_labels[idx]
                if life > 0:
                    alpha = life / float(POPUP_LIFETIME)
                    qc = get_score_color(val, -1.0, 1.0)
                    line.set_xdata([ms, ms])
                    line.set_color(qc)
                    line.set_alpha(alpha * 0.8)
                    line.set_visible(True)

                    label.set_position((ms, (val + 1) / 2))
                    label.set_text(f"{val:+.2f}")
                    label.set_color(qc)
                    label.set_alpha(alpha)
                    label.set_visible(True)
                    q[1] -= 1
                if q[1] <= 0:
                    active_qualifiers.remove(q)

            # Handle Debug Console
            for i, debug in enumerate(active_debug_lines[:]):
                if debug['lifetime'] > 0:
                    txt_artist = debug_console_pool[i]
                    txt_artist.set_text(debug['text'])
                    txt_artist.set_color(colors[debug['band_idx']])
                    txt_artist.set_alpha(min(1.0, debug['lifetime'] / 10.0)) # Quick fade out at the end
                    txt_artist.set_visible(True)
                    debug['lifetime'] -= 1
                else:
                    active_debug_lines.remove(debug)

            # Optimization: Only return visible artists for blitting
            changed_artists = [playhead_transient, cleanup_transient, buffer_line, mean_line,
                              metrics_text, rating_text, score_display_text]
            if highest_peak_line.get_visible(): changed_artists.append(highest_peak_line)
            if pool_snap_lines.get_visible(): changed_artists.append(pool_snap_lines)

            changed_artists.extend(threshold_lines)
            changed_artists.extend(transient_lines)

            for p in pool_snapshots:
                if p.get_visible(): changed_artists.append(p)
            for s in pool_scores:
                if s.get_visible(): changed_artists.append(s)
            for l in pool_qualifier_lines:
                if l.get_visible(): changed_artists.append(l)
            for t in pool_qualifier_labels:
                if t.get_visible(): changed_artists.append(t)
            for st in pool_snap_texts:
                if st.get_visible(): changed_artists.append(st)
            for d in debug_console_pool:
                if d.get_visible(): changed_artists.append(d)

            return changed_artists

        # Select analysis frames that correspond exactly to 30 FPS video timing
        duration = times[-1]
        fps = 30
        num_video_frames = int(duration * fps)
        video_frame_times = np.arange(num_video_frames) / float(fps)
        # times is a list here, convert to np.array for searchsorted
        frame_indices = np.searchsorted(np.array(times), video_frame_times)
        num_frames = len(frame_indices)

        ani = animation.FuncAnimation(fig, update, frames=frame_indices, blit=True, interval=1000.0/fps)

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
    peaks_params_list = [{'thresh_vals': [], 'left_mins': [], 'right_mins': [], 'proms': []} for _ in range(4)]

    for p in all_peaks:
        b = p['band_idx']
        peaks_list[b].append(p['p_idx'])
        params = peaks_params_list[b]
        params['thresh_vals'].append(p.get('thresh_val', 0.0))
        params['left_mins'].append(p.get('left_min', 0.0))
        params['right_mins'].append(p.get('right_min', 0.0))
        params['proms'].append(p.get('prominence', 0.0))

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

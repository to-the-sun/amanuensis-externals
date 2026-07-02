import argparse
import librosa
import numpy as np
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
    if _initialized: return
    with _init_lock:
        if _initialized: return
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
    if score == 0: return "#808080"
    if score < 0:
        t = score / min_score if min_score < 0 else 0.0
        t = max(0, min(1, t))
        r = int(0x80 + (0xff - 0x80) * t)
        g = int(0x80 + (0x00 - 0x80) * t)
        b = int(0x80 + (0x00 - 0x80) * t)
    else:
        t = score / max_score if max_score > 0 else 0.0
        t = max(0, min(1, t))
        r = int(0x80 + (0x00 - 0x80) * t)
        g = int(0x80 + (0xff - 0x80) * t)
        b = int(0x80 + (0x00 - 0x80) * t)
    return f"#{r:02x}{g:02x}{b:02x}"

def generate_video(audio_path, data):
    ensure_initialized()
    if cumulative_transience is None: raise ImportError("The 'cumulative_transience' extension module could not be loaded.")
    print(f"Generating video for {audio_path}...")
    try:
        times = data['times']; onset_envs = data['onset_envs']; rolling_thresholds = data['rolling_thresholds']
        all_peaks = []
        for b_peaks in data['peaks']: all_peaks.extend(b_peaks)
        all_peaks.sort(key=lambda x: x['p_idx'])
        max_peak = data['max_peak_value']; min_score_seen = data['min_score_seen']; max_score_seen = data['max_score_seen']
        ratings = data['ratings']; std_devs = data['std_devs']; means = data['means']; contrasts = data['contrasts']; peak_stds = data['peak_stds']
        fig, (ax_transient, ax_snapshot, ax_buf) = plt.subplots(3, 1, figsize=(12, 14), gridspec_kw={'height_ratios': [1, 0.4, 1]})
        colors = ['#1b4f72', '#3498db', '#2ecc71', '#a9dfbf']; alphas = [1.0, 0.8, 0.6, 0.4]; labels = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble']
        transient_lines = []; threshold_lines = []
        for i in range(4):
            line, = ax_transient.plot(times, onset_envs[i], color=colors[i], lw=2, alpha=alphas[i], label=labels[i], zorder=2); transient_lines.append(line)
            t_line, = ax_transient.plot([times[0], times[-1]], [0, 0], color=colors[i], lw=1, ls='--', alpha=0.5, zorder=3); threshold_lines.append(t_line)
        playhead_transient = ax_transient.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead', zorder=15)
        cleanup_transient = ax_transient.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep', zorder=15)
        ax_transient.set_title(f"4-Band Transient Analysis - {os.path.basename(audio_path)}"); ax_transient.set_ylabel("Onset Strength"); ax_transient.grid(True, alpha=0.3); ax_transient.set_xlim(-20, 5)
        def format_time(x, pos):
            m = int(abs(x) // 60); s = int(abs(x) % 60); prefix = "-" if x < 0 else ""; return f"{prefix}{m}:{s:02d}"
        ax_transient.xaxis.set_major_formatter(ticker.FuncFormatter(format_time)); ax_transient.set_ylim(0, max_peak * 1.1 if max_peak > 0 else 1)
        buffer_times = np.linspace(-5000, 0, 5001); buffer_line, = ax_buf.plot(buffer_times, np.zeros(5001), color='#f1c40f', lw=2); mean_line, = ax_buf.plot([-5000, 0], [0, 0], color='#808080', lw=1, ls='--', alpha=0.5, label='Mean Energy')
        ax_buf.set_title("Accumulated 5s Historical Buffer"); ax_buf.set_xlabel("Time Relative to Peak (ms)"); ax_buf.set_ylabel("Accumulated Energy"); ax_buf.grid(True, alpha=0.3); ax_buf.set_xlim(-5000, 0); ax_buf.set_ylim(0, 1)
        highest_peak_line = ax_buf.axvline(0, color='#f1c40f', lw=2, ls='--', visible=False, zorder=15)
        ax_snapshot.set_xlim(-45, 1); ax_snapshot.set_ylim(-0.5, 3.5); ax_snapshot.set_yticks([0, 1, 2, 3]); ax_snapshot.set_yticklabels(['Sub', 'Bass', 'Mid', 'Hi'], fontsize=10, fontweight='bold'); ax_snapshot.set_title("39ms Rolling Window Snapshot", fontsize=14, fontweight='bold'); ax_snapshot.set_xlabel("Time Relative to Latest Peak (ms)", fontsize=12); ax_snapshot.grid(False)
        for i in range(3): ax_snapshot.axhline(i + 0.5, color='gray', lw=1, alpha=0.3)
        POPUP_LIFETIME = 60; MAX_POOL = 128; snap_verts_x = np.concatenate([[buffer_times[0]], buffer_times, [buffer_times[-1]]])
        pool_scores = [ax_transient.text(0, 0, '', visible=False) for _ in range(MAX_POOL)]; pool_qualifier_lines = [ax_buf.axvline(0, visible=False, lw=3.0, ls=':', alpha=0.8) for _ in range(MAX_POOL)]; pool_qualifier_labels = [ax_buf.text(0, 0, '', visible=False, fontsize=8, transform=ax_buf.get_xaxis_transform()) for _ in range(MAX_POOL)]; snapshot_line, = ax_buf.plot(buffer_times, np.zeros(5001), color='#2ecc71', lw=2, label='Current Snapshot', zorder=10)
        MAX_SNAPSHOT_POOL = 32; from matplotlib.collections import LineCollection; pool_snap_lines = LineCollection([], colors=[], linewidths=3, visible=False); ax_snapshot.add_collection(pool_snap_lines); pool_snap_texts = [ax_snapshot.text(0, 0, '', visible=False, fontsize=13, va='center', ha='right', fontweight='bold') for _ in range(MAX_SNAPSHOT_POOL)]
        active_flashes = []; active_scores = []; active_qualifiers = []; live_peaks_x = []; live_peaks_y = []
        live_peaks_scatter = ax_transient.scatter([], [], color='#f1c40f', marker='x', s=50, alpha=1.0, zorder=11)
        current_snapshot_avg = 0.0; rolling_window_scores = []; last_snapshot_display = None
        score_display_text = ax_transient.text(0.02, 0.98, 'Score: +0.00', transform=ax_transient.transAxes, verticalalignment='top', fontsize=20, color='#808080', fontweight='bold', zorder=20)
        rating_text = ax_transient.text(0.02, 0.90, 'Rating: 0.00', transform=ax_transient.transAxes, verticalalignment='top', fontsize=12, color='#f1c40f', fontweight='bold', zorder=20)
        MAX_DEBUG_LINES = 10; debug_console_pool = [ax_transient.text(0.98, 0.98 - (i * 0.05), '', transform=ax_transient.transAxes, verticalalignment='top', horizontalalignment='right', fontsize=12, family='monospace', color='#2ecc71', fontweight='bold', visible=False, zorder=20) for i in range(MAX_DEBUG_LINES)]; active_debug_lines = []
        metrics_text = ax_buf.text(0.02, 0.95, '', transform=ax_buf.transAxes, verticalalignment='top', fontsize=10, color='#f1c40f', fontweight='bold')
        accumulated_buffer = np.zeros(5001); last_frame_processed = -1; peak_search_ptr = 0; active_buffer_peaks = []

        def update(frame):
            nonlocal last_frame_processed, current_snapshot_avg, rolling_window_scores, active_debug_lines, peak_search_ptr, accumulated_buffer, last_snapshot_display, active_buffer_peaks
            for s in pool_scores: s.set_visible(False)
            for l in pool_qualifier_lines: l.set_visible(False)
            for t in pool_qualifier_labels: t.set_visible(False)
            for d in debug_console_pool: d.set_visible(False)
            pool_snap_lines.set_visible(False)
            for st in pool_snap_texts: st.set_visible(False)
            cleanup_frame = frame - 15000
            while active_buffer_peaks and active_buffer_peaks[0]['p_idx'] <= cleanup_frame:
                p_old = active_buffer_peaks.pop(0)
                accumulated_buffer -= p_old['snapshot']
            new_peaks = []
            while peak_search_ptr < len(all_peaks) and all_peaks[peak_search_ptr]['p_idx'] <= frame:
                if all_peaks[peak_search_ptr]['p_idx'] > last_frame_processed: new_peaks.append(all_peaks[peak_search_ptr])
                peak_search_ptr += 1
            for p in new_peaks:
                rolling_window_scores.append({'frame': p['p_idx'], 'score': p['total_score'], 'band_idx': p['band_idx']})
                accumulated_buffer += p['snapshot']; active_buffer_peaks.append(p); q_sum = sum(q['val'] for q in p['qualifiers']); f_val = p.get('detected_peak_val', p['peak_val'])
                debug_msg = (f"[B{p['band_idx']}] (Flux:{f_val:.2f} > Th:{p['thresh_val']:.2f} & Flux >= 0.0 & Pr:{p['prominence']:.2f} > MidPt) | Score:{p['total_score']:+.2f} = {p['peak_val']:.2f} * {q_sum:.2f}")
                active_debug_lines.insert(0, {'text': debug_msg, 'lifetime': POPUP_LIFETIME, 'band_idx': p['band_idx']})
                if len(active_debug_lines) > MAX_DEBUG_LINES: active_debug_lines = active_debug_lines[:MAX_DEBUG_LINES]
                active_qualifiers.clear()
                for q_info in p['qualifiers']:
                    if len(active_qualifiers) < MAX_POOL: active_qualifiers.append([len(active_qualifiers), POPUP_LIFETIME, q_info['ms'], q_info['val']])
                for i in range(MAX_POOL):
                    if not any(a[0] == i for a in active_scores): active_scores.append([i, POPUP_LIFETIME, p['peak_val'], p['total_score'], p['time']]); break
                snapshot_line.set_ydata(p['snapshot'])
                live_peaks_x.append(p['time']); live_peaks_y.append(p['peak_val']); live_peaks_scatter.set_offsets(np.c_[live_peaks_x, live_peaks_y])
            last_frame_processed = frame; rolling_window_scores = [s for s in rolling_window_scores if s['frame'] > frame - 39]
            if rolling_window_scores:
                current_snapshot_avg = sum(s['score'] for s in rolling_window_scores) / len(rolling_window_scores); latest_p_frame = max(s['frame'] for s in rolling_window_scores); segments = []; seg_colors = []; snap_data = []
                for i, s in enumerate(rolling_window_scores[:MAX_SNAPSHOT_POOL]):
                    rel_ms = float(s['frame'] - latest_p_frame); score_val = s['score']; band_idx = s['band_idx']; lane_y = band_idx; score_c = get_score_color(score_val, min_score_seen, max_score_seen)
                    segments.append([[rel_ms, lane_y - 0.4], [rel_ms, lane_y + 0.4]]); seg_colors.append(colors[band_idx])
                    snap_data.append(((rel_ms - 0.8, lane_y), f"{score_val:+.2f}", score_c))
                last_snapshot_display = (segments, seg_colors, snap_data)

            if last_snapshot_display:
                segs, cols, s_data = last_snapshot_display
                pool_snap_lines.set_segments(segs); pool_snap_lines.set_colors(cols); pool_snap_lines.set_visible(True)
                for i, (pos, txt_val, color) in enumerate(s_data):
                    txt = pool_snap_texts[i]; txt.set_position(pos); txt.set_text(txt_val); txt.set_color(color); txt.set_visible(True)

            current_time = times[frame]; ax_transient.set_xlim(current_time - 20, current_time + 5)
            t_start = current_time - 20; t_end = current_time + 5
            idx_start = np.searchsorted(times, t_start); idx_end = np.searchsorted(times, t_end)
            local_max = 0.0
            for b_env in onset_envs:
                if idx_start < len(b_env):
                    seg = b_env[idx_start:idx_end]
                    if len(seg) > 0:
                        m = np.max(seg)
                        if m > local_max: local_max = m
            ax_transient.set_ylim(0, max(1.0, local_max * 1.1))
            for i in range(4): threshold_lines[i].set_ydata([rolling_thresholds[i][frame], rolling_thresholds[i][frame]]); threshold_lines[i].set_xdata([current_time - 20, current_time + 5])
            playhead_transient.set_xdata([current_time, current_time]); cleanup_transient.set_xdata([current_time - 15, current_time - 15]); buffer_line.set_ydata(accumulated_buffer);
            # Exclude the last 99ms to avoid self-referential bias from the peak at zero.
            current_max = np.max(accumulated_buffer[:-99]) if len(accumulated_buffer) > 99 else 0; ax_buf.set_ylim(0, max(0.1, current_max * 1.1)); mean_line.set_ydata([means[frame], means[frame]]); metrics_text.set_text(f"Std Dev: {std_devs[frame]:.3f}\nContrast: {contrasts[frame]:.3f}\nPeak Std: {peak_stds[frame]:.3f}"); rating_text.set_text(f"Rating: {ratings[frame]:.2f}"); score_display_text.set_text(f"Score: {current_snapshot_avg:+.2f}"); score_display_text.set_color(get_score_color(current_snapshot_avg, min_score_seen, max_score_seen))
            if metrics_out := data.get('metrics'): # fallback if not in data directly
                h_peak_ms = data.get('highest_peaks_ms', [None]*len(times))[frame] # dummy check
            # In generate_video, data['metrics'] isn't available frame-by-frame easily unless we pass it.
            # However, we have access to means, std_devs, etc. We need highest_peak_ms.
            # Let's check if it's in data.
            h_peaks = data.get('highest_peaks_ms')
            if h_peaks is not None:
                hp = h_peaks[frame]
                if hp is not None:
                    highest_peak_line.set_xdata([hp, hp])
                    highest_peak_line.set_visible(True)
                else:
                    highest_peak_line.set_visible(False)
            current_ylim = ax_transient.get_ylim()[1]
            for score in active_scores[:]:
                idx, life, initial_y, val, p_time = score; txt = pool_scores[idx]
                if life > 0: progress = (POPUP_LIFETIME - life) / float(POPUP_LIFETIME); txt.set_position((p_time, initial_y + (progress * 0.1 * current_ylim))); txt.set_text(f"{val:+.2f}"); txt.set_alpha(life / float(POPUP_LIFETIME)); txt.set_color(get_score_color(val, min_score_seen, max_score_seen)); txt.set_visible(True); score[1] -= 1
                else: active_scores.remove(score)
            for q in active_qualifiers[:]:
                idx, life, ms, val = q; line = pool_qualifier_lines[idx]; label = pool_qualifier_labels[idx]
                if life > 0: alpha = life / float(POPUP_LIFETIME); qc = get_score_color(val, -1.0, 1.0); line.set_xdata([ms, ms]); line.set_color(qc); line.set_alpha(alpha * 0.8); line.set_visible(True); label.set_position((ms, (val + 1) / 2)); label.set_text(f"{val:+.2f}"); label.set_color(qc); label.set_alpha(alpha); label.set_visible(True); q[1] -= 1
                else: active_qualifiers.remove(q)
            for i, debug in enumerate(active_debug_lines[:]):
                if debug['lifetime'] > 0: txt_artist = debug_console_pool[i]; txt_artist.set_text(debug['text']); txt_artist.set_color(colors[debug['band_idx']]); txt_artist.set_alpha(min(1.0, debug['lifetime'] / 10.0)); txt_artist.set_visible(True); debug['lifetime'] -= 1
                else: active_debug_lines.remove(debug)
            changed_artists = [playhead_transient, cleanup_transient, buffer_line, snapshot_line, mean_line, highest_peak_line, metrics_text, rating_text, score_display_text, live_peaks_scatter] + threshold_lines + transient_lines
            if pool_snap_lines.get_visible(): changed_artists.append(pool_snap_lines)
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

        duration = times[-1]; fps = 30; num_video_frames = int(duration * fps); video_frame_times = np.arange(num_video_frames) / float(fps); frame_indices = np.searchsorted(np.array(times), video_frame_times)
        ani = animation.FuncAnimation(fig, update, frames=frame_indices, blit=True, interval=1000.0/fps)
        with tempfile.NamedTemporaryFile(suffix=".mp4", delete=False) as tmp: temp_video_path = tmp.name
        pbar = tqdm(total=len(frame_indices), desc="Rendering Video", unit="frame"); writer = animation.FFMpegWriter(fps=30, metadata=dict(artist='Transient Analysis Tool'), bitrate=2000)
        fig.tight_layout(pad=1.5); ani.save(temp_video_path, writer=writer, progress_callback=lambda i, n: pbar.update(1))
        pbar.close(); plt.close(fig)

        # Recording metrics to [audio_file]_ratings.txt in same folder
        try:
            song_name = os.path.splitext(os.path.basename(audio_path))[0]
            project_dir = rf'D:\[Library]\[Audio]\[Works]\[Projects]\{song_name}'
            if os.path.exists(project_dir):
                ratings_file = os.path.join(project_dir, 'ratings.txt')
                with open(ratings_file, 'w', encoding='utf-8') as f:
                    f.write(f"Rating: {ratings[-1]:.2f}\n")
                    f.write(f"Standard Deviation: {std_devs[-1]:.3f}\n")
                    f.write(f"Contrast: {contrasts[-1]:.3f}\n")
                    f.write(f"Bar Length Deviation: {peak_stds[-1]:.3f}\n")
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
            print(f"Video generated: {output_video}"); return output_video
        else: print("Error: ffmpeg not found."); return None
    except Exception as e: traceback.print_exc(); return None

def analyze_audio_unified(file_path):
    ensure_initialized()
    if cumulative_transience is None: raise ImportError("The 'cumulative_transience' extension module could not be loaded.")
    print(f"Analyzing {file_path}...")
    y, sr = librosa.load(file_path, sr=44100, mono=True); res = cumulative_transience.analyze_audio(y, sr); return res

def main():
    ensure_initialized()
    parser = argparse.ArgumentParser(description="Standalone transient analysis and video generation.")
    parser.add_argument("files", nargs="*", help="Optional list of audio files to process.")
    args = parser.parse_args(); extensions = ('.wav', '.mp3', '.m4a', '.flac', '.ogg', '.aiff'); audio_files = []
    sources = args.files if args.files else [os.getcwd()]
    for source in sources:
        if os.path.isdir(source): audio_files.extend([os.path.join(source, f) for f in os.listdir(source) if f.lower().endswith(extensions)])
        elif os.path.isfile(source) and source.lower().endswith(extensions): audio_files.append(source)
    audio_files.sort()
    for f in audio_files:
        if not os.path.exists(f): continue
        result = analyze_audio_unified(f)
        if result: generate_video(f, result)

if __name__ == "__main__":
    try:
        main()
        print("\nAnalysis complete.")
    except Exception:
        print("\n" + "="*60)
        print("ERROR ENCOUNTERED DURING ANALYSIS")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    except BaseException:
        # Catch KeyboardInterrupt, SystemExit, etc.
        traceback.print_exc()
    finally:
        # Keep window open for user to see output/errors
        print("\nPersistence check: The script will remain open until you press Enter.")
        try:
            input("\nPress Enter to exit...")
        except (EOFError, KeyboardInterrupt):
            pass

import os
import sys
import argparse
import traceback
import numpy as np
import soundfile as sf
import librosa
from scipy.spatial.distance import cdist

try:
    import ct_utils
except ImportError:
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import ct_utils

# Global defaults
MIN_SEGMENT_LEN_MS = 100
ATOM_ITERATION_MS = 50
SIMILARITY_THRESHOLD = 0.75  # Normalized similarity threshold (0.0 to 1.0)


def extract_frame_features(y, sr, hop_length=160):
    """
    Extract frame-level features (MFCC + Delta + Chroma) for DTW comparison.
    hop_length=160 samples at 16kHz corresponds to 10ms per frame.
    """
    # Force 16kHz for uniform temporal resolution
    if sr != 16000:
        y = librosa.resample(y, orig_sr=sr, target_sr=16000)
        sr = 16000

    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13, hop_length=hop_length)
    mfcc_delta = librosa.feature.delta(mfcc)
    chroma = librosa.feature.chroma_stft(y=y, sr=sr, hop_length=hop_length)

    # Stack features along feature axis: (n_frames, n_features)
    features = np.vstack([mfcc, mfcc_delta, chroma]).T

    # Normalize features (zero mean, unit variance per feature dimension)
    mean = np.mean(features, axis=0, keepdims=True)
    std = np.std(features, axis=0, keepdims=True) + 1e-8
    features = (features - mean) / std

    frame_duration_ms = (hop_length / sr) * 1000.0
    return y, sr, features, frame_duration_ms


def ensure_ct_initialized():
    try:
        ct_utils.ensure_extension_built()
        import cumulative_transience as ct
        return ct
    except Exception as e:
        print(f"Warning: Could not initialize cumulative_transience: {e}")
        return None


def find_most_common_cluster_center(diffs_ms):
    """
    Calculates the center point of the most common cluster of floating-point difference values.
    Uses Gaussian Kernel Density Estimation (KDE) with fallback to histogram mode binning.
    """
    if not diffs_ms:
        return float(MIN_SEGMENT_LEN_MS)
    if len(diffs_ms) == 1:
        return float(diffs_ms[0])

    diffs_arr = np.array(diffs_ms, dtype=np.float64)
    min_v = float(np.min(diffs_arr))
    max_v = float(np.max(diffs_arr))

    if min_v == max_v:
        return min_v

    try:
        from scipy.stats import gaussian_kde
        kde = gaussian_kde(diffs_arr)
        grid = np.linspace(min_v, max_v, 1000)
        density = kde(grid)
        max_idx = np.argmax(density)
        peak_val = grid[max_idx]

        # Calculate centroid of values around the peak (window of 5% range or min 10ms)
        window = max(10.0, (max_v - min_v) * 0.05)
        cluster_mask = np.abs(diffs_arr - peak_val) <= window
        cluster_points = diffs_arr[cluster_mask]

        if len(cluster_points) > 0:
            return float(np.mean(cluster_points))
        return float(peak_val)
    except Exception as e:
        # Fallback to histogram mode binning
        num_bins = max(10, min(50, len(diffs_arr) // 2))
        counts, bin_edges = np.histogram(diffs_arr, bins=num_bins)
        max_bin = np.argmax(counts)
        bin_low, bin_high = bin_edges[max_bin], bin_edges[max_bin + 1]
        cluster_points = diffs_arr[(diffs_arr >= bin_low) & (diffs_arr <= bin_high)]
        if len(cluster_points) > 0:
            return float(np.mean(cluster_points))
        return float((bin_low + bin_high) / 2.0)


def detect_transient_candidate_lengths(y, sr, min_seg_ms=MIN_SEGMENT_LEN_MS, atom_ms=ATOM_ITERATION_MS, max_seg_ms=None, gui_mode=True):
    """
    Detects transients across the audio file using cumulative_transience peak detection,
    computes full floating-point pairwise time differences between all transients,
    plots them on a pop-up scatter plot, marks the center point of the most common cluster,
    and returns a list containing that single cluster center segment length.
    """
    print("Detecting transients across audio file using cumulative_transience peak detection...")
    ct = ensure_ct_initialized()
    onset_times_ms = []

    if ct is not None:
        try:
            analysis_res = ct.analyze_audio(y.astype(np.float32), int(sr))
            if analysis_res and 'peaks' in analysis_res:
                for band_peaks in analysis_res['peaks']:
                    for p in band_peaks:
                        onset_times_ms.append(p['time'] * 1000.0)
                onset_times_ms = sorted(onset_times_ms)
        except Exception as e:
            print(f"Error running cumulative_transience peak detection: {e}")
            onset_times_ms = []

    print(f"Detected {len(onset_times_ms)} transients.")

    if len(onset_times_ms) < 2:
        print("Not enough transients detected. Falling back to default segment length.")
        return [float(min_seg_ms)]

    raw_diffs = []
    total_dur_ms = (len(y) / sr) * 1000.0
    upper_ms = total_dur_ms / 2.0 if max_seg_ms is None else min(total_dur_ms / 2.0, max_seg_ms)

    # Assess full floating-point time differences between all transient pairs
    for i in range(len(onset_times_ms)):
        for j in range(i + 1, len(onset_times_ms)):
            diff_ms = float(abs(onset_times_ms[j] - onset_times_ms[i]))
            if min_seg_ms <= diff_ms <= upper_ms:
                raw_diffs.append(diff_ms)

    if not raw_diffs:
        print("No valid transient interval candidates found in range. Falling back to default segment length.")
        return [float(min_seg_ms)]

    # Calculate center point of the most common cluster of floating-point values
    center_point_ms = find_most_common_cluster_center(raw_diffs)
    print(f"Accumulated {len(raw_diffs)} peak difference values.")
    print(f"Calculated most common cluster center point: {center_point_ms:.2f} ms")

    # Graph all floating-point differences on a histogram (bar graph) with marked cluster center
    if gui_mode:
        try:
            import matplotlib.pyplot as plt
            fig, ax = plt.subplots(figsize=(10, 6))
            diffs_arr = np.array(raw_diffs, dtype=np.float64)

            # Choose appropriate small bin interval (~10ms bins or dynamic bin count)
            min_v, max_v = float(np.min(diffs_arr)), float(np.max(diffs_arr))
            range_v = max(1.0, max_v - min_v)
            num_bins = max(20, min(100, int(range_v / 10.0)))

            counts, bin_edges, patches = ax.hist(
                diffs_arr,
                bins=num_bins,
                color='#3498db',
                edgecolor='#2980b9',
                alpha=0.75,
                rwidth=0.85,
                label='Time Differences Count'
            )

            max_count = float(np.max(counts)) if len(counts) > 0 else 1.0
            ax.axvline(
                center_point_ms,
                color='#e74c3c',
                linestyle='--',
                linewidth=2,
                label=f'Most Common Cluster Center ({center_point_ms:.2f} ms)'
            )
            ax.scatter(
                [center_point_ms],
                [max_count],
                color='#e74c3c',
                s=120,
                zorder=5,
                marker='X',
                label='Cluster Center Marker'
            )

            ax.set_title("Transient Pairwise Time Differences Distribution", fontsize=12, fontweight='bold')
            ax.set_xlabel("Time Difference (ms)", fontsize=10)
            ax.set_ylabel("Count / Frequency", fontsize=10)
            ax.grid(True, alpha=0.3)
            ax.legend(loc='upper right')
            fig.tight_layout()
            plt.show()
        except Exception as e:
            print(f"Could not display histogram plot GUI: {e}")

    return [center_point_ms]


def compute_segment_dtw_similarity(feat1, feat2):
    """
    Computes DTW similarity score between two segment feature matrices feat1 and feat2.
    Returns normalized similarity score in range [0, 1].
    """
    if len(feat1) == 0 or len(feat2) == 0:
        return 0.0

    # Compute pairwise cosine distance matrix
    dist_matrix = cdist(feat1, feat2, metric='cosine')

    # Run DTW
    D, wp = librosa.sequence.dtw(C=dist_matrix)
    path_len = len(wp)
    if path_len == 0:
        return 0.0

    norm_cost = D[-1, -1] / path_len
    # Convert distance to similarity score in [0, 1]
    similarity = max(0.0, 1.0 - norm_cost)
    return similarity


def analyze_segment_length(y, sr, features, frame_dur_ms, seg_len_ms, similarity_threshold=SIMILARITY_THRESHOLD, gui_callback=None):
    """
    Divides audio into contiguous segments of seg_len_ms and analyzes each segment
    against each other using Dynamic Time Warping.
    Returns (patterns, total_pattern_duration_ms)
    """
    frames_per_seg = max(1, int(round(seg_len_ms / frame_dur_ms)))
    n_frames = len(features)
    num_segments = n_frames // frames_per_seg

    if num_segments < 2:
        return [], 0.0

    segments = []
    for i in range(num_segments):
        start_f = i * frames_per_seg
        end_f = (i + 1) * frames_per_seg
        segments.append({
            'index': i,
            'start_ms': start_f * frame_dur_ms,
            'end_ms': end_f * frame_dur_ms,
            'features': features[start_f:end_f]
        })

    matched_segment_indices = set()
    matches = []

    for i in range(num_segments):
        for j in range(i + 1, num_segments):
            sim = compute_segment_dtw_similarity(segments[i]['features'], segments[j]['features'])

            if gui_callback:
                gui_callback(seg_len_ms, segments[i], segments[j], sim)

            if sim >= similarity_threshold:
                matched_segment_indices.add(i)
                matched_segment_indices.add(j)
                matches.append((i, j, sim))

    if not matched_segment_indices:
        return [], 0.0

    # Group contiguous matched segments into patterns
    sorted_matched = sorted(list(matched_segment_indices))
    pattern_groups = []
    current_group = [sorted_matched[0]]

    for idx in sorted_matched[1:]:
        if idx == current_group[-1] + 1:
            current_group.append(idx)
        else:
            pattern_groups.append(current_group)
            current_group = [idx]
    pattern_groups.append(current_group)

    patterns = []
    total_pattern_duration_ms = 0.0

    for group in pattern_groups:
        if len(group) < 2:
            continue
        start_idx = group[0]
        end_idx = group[-1]
        start_ms = segments[start_idx]['start_ms']
        end_ms = segments[end_idx]['end_ms']
        duration_ms = end_ms - start_ms

        patterns.append({
            'start_ms': start_ms,
            'end_ms': end_ms,
            'duration_ms': duration_ms,
            'segments': group
        })
        total_pattern_duration_ms += duration_ms

    return patterns, total_pattern_duration_ms


def find_patterns(audio_path, min_segment_ms=MIN_SEGMENT_LEN_MS, atom_iteration_ms=ATOM_ITERATION_MS, similarity_threshold=SIMILARITY_THRESHOLD, max_len_ms=None, gui_mode=True):
    """
    Full pipeline to search for patterns across transient-derived segment lengths,
    determine optimal bar length, and export pattern WAV files.
    """
    if not os.path.exists(audio_path):
        raise FileNotFoundError(f"Audio file not found: {audio_path}")

    print(f"Loading audio file: {audio_path}")
    raw_y, orig_sr = sf.read(audio_path)
    if raw_y.ndim > 1:
        raw_y_mono = np.mean(raw_y, axis=1)
    else:
        raw_y_mono = raw_y

    # Extract audio and features at 16kHz
    y, sr, features, frame_dur_ms = extract_frame_features(raw_y_mono, orig_sr)
    total_duration_ms = (len(y) / sr) * 1000.0

    print(f"Audio duration: {total_duration_ms:.2f} ms ({total_duration_ms/1000.0:.2f} s)")

    # Conduct transient detection to find candidate segment length (center of primary cluster)
    segment_lengths = detect_transient_candidate_lengths(
        y=y,
        sr=sr,
        min_seg_ms=min_segment_ms,
        atom_ms=atom_iteration_ms,
        max_seg_ms=max_len_ms,
        gui_mode=gui_mode
    )

    best_bar_length = None
    max_total_pattern_len_ms = -1.0
    best_patterns = []
    results = {}

    for seg_len in segment_lengths:
        print(f"\nAnalyzing target segment length: {seg_len:.2f} ms...")
        patterns, total_pat_len_ms = analyze_segment_length(
            y, sr, features, frame_dur_ms, seg_len,
            similarity_threshold=similarity_threshold
        )

        results[seg_len] = {
            'patterns': patterns,
            'total_pattern_len_ms': total_pat_len_ms
        }

        print(f"Segment length {seg_len:.2f} ms: Found {len(patterns)} patterns, Total Duration = {total_pat_len_ms:.2f} ms")

        if total_pat_len_ms > max_total_pattern_len_ms and total_pat_len_ms > 0:
            max_total_pattern_len_ms = total_pat_len_ms
            best_bar_length = seg_len
            best_patterns = patterns

    if best_bar_length is None or not best_patterns:
        print("\nNo repeating patterns were detected across tested segment lengths.")
        return

    print("\n" + "="*60)
    print(f"ANALYSIS COMPLETE: Bar length determined to be {best_bar_length:.2f} ms")
    print(f"Total pattern duration at bar length: {max_total_pattern_len_ms:.2f} ms")
    print(f"Number of patterns identified: {len(best_patterns)}")
    print("="*60)

    # Graph waveform with highlighted patterns and segment boundaries
    if gui_mode and best_patterns:
        try:
            import matplotlib.pyplot as plt

            fig, ax = plt.subplots(figsize=(12, 6))
            time_axis_s = np.linspace(0, total_duration_ms / 1000.0, len(y))
            ax.plot(time_axis_s, y, color='#2c3e50', alpha=0.6, linewidth=0.8, label='Waveform')

            colors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22', '#3498db']
            y_max = float(np.max(y)) if len(y) > 0 else 1.0

            frames_per_seg = max(1, int(round(best_bar_length / frame_dur_ms)))

            for pat_idx, pat in enumerate(best_patterns, 1):
                color = colors[(pat_idx - 1) % len(colors)]
                pat_start_s = pat['start_ms'] / 1000.0
                pat_end_s = pat['end_ms'] / 1000.0

                # Highlight pattern span
                ax.axvspan(pat_start_s, pat_end_s, color=color, alpha=0.35,
                           label=f"Pattern {pat_idx} ({pat['duration_ms']:.0f} ms)")

                # Mark constituent segments
                for seg_i, seg_idx in enumerate(pat['segments']):
                    s_start_ms = seg_idx * frames_per_seg * frame_dur_ms
                    s_end_ms = (seg_idx + 1) * frames_per_seg * frame_dur_ms
                    s_start_s = s_start_ms / 1000.0
                    s_end_s = s_end_ms / 1000.0

                    ax.axvline(s_start_s, color=color, linestyle='--', alpha=0.7, linewidth=1.2)
                    if seg_i == len(pat['segments']) - 1:
                        ax.axvline(s_end_s, color=color, linestyle='--', alpha=0.7, linewidth=1.2)

                    mid_s = (s_start_s + s_end_s) / 2.0
                    ax.text(mid_s, y_max * 0.85, f"Seg {seg_idx}", color=color, fontsize=9,
                            fontweight='bold', ha='center', va='center',
                            bbox=dict(boxstyle='round,pad=0.2', facecolor='white', alpha=0.75, edgecolor=color))

            ax.set_title(f"Audio Waveform & Detected Patterns (Bar Length: {best_bar_length:.2f} ms)",
                         fontsize=12, fontweight='bold')
            ax.set_xlabel("Time (seconds)", fontsize=10)
            ax.set_ylabel("Amplitude", fontsize=10)
            ax.grid(True, alpha=0.3)
            ax.legend(loc='upper right')
            fig.tight_layout()
            plt.show()
        except Exception as e:
            print(f"Could not display pattern waveform plot GUI: {e}")

    return best_bar_length, best_patterns


def main():
    parser = argparse.ArgumentParser(description="Find audio patterns and bar length using transient-guided Dynamic Time Warping (DTW).")
    parser.add_argument("audio_file", nargs="?", help="Path to input audio file.")
    parser.add_argument("--min-segment", type=int, default=MIN_SEGMENT_LEN_MS, help="Minimum segment length in ms (default: 100ms).")
    parser.add_argument("--atom-iteration", type=int, default=ATOM_ITERATION_MS, help="Atom of iteration in ms (default: 50ms).")
    parser.add_argument("--threshold", type=float, default=SIMILARITY_THRESHOLD, help="DTW similarity threshold (default: 0.75).")
    parser.add_argument("--max-len", type=int, default=None, help="Maximum segment length to test in ms.")
    parser.add_argument("--no-gui", action="store_true", help="Run in headless mode without GUI visualization.")

    args = parser.parse_args()

    audio_path = args.audio_file
    if not audio_path:
        current_dir = os.path.dirname(os.path.abspath(__file__))
        extensions = ('.wav', '.mp3', '.flac', '.ogg', '.aiff')
        files = [os.path.join(current_dir, f) for f in os.listdir(current_dir) if f.lower().endswith(extensions)]
        files.sort()
        if files:
            audio_path = files[0]
            print(f"No audio file specified. Defaulting to: {audio_path}")
        else:
            print("Error: No audio file specified or found in current directory.")
            return

    gui_mode = not args.no_gui
    find_patterns(
        audio_path=audio_path,
        min_segment_ms=args.min_segment,
        atom_iteration_ms=args.atom_iteration,
        similarity_threshold=args.threshold,
        max_len_ms=args.max_len,
        gui_mode=gui_mode
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("\n" + "="*60)
        print("ERROR OCCURRED")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    finally:
        try:
            input("\nPress Enter to exit...")
        except EOFError:
            pass

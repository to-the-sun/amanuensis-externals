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


def extract_frame_features(y, sr, analysis_res=None, hop_ms=10):
    """
    Extract frame-level features based on the prominence of the smooth transience
    for DTW comparison using cumulative_transience peak detection results.
    hop_ms=10 downsamples the 1ms transience envelopes to ~10ms frame resolution (100 Hz).
    """
    if analysis_res is None:
        ct = ensure_ct_initialized()
        if ct is None:
            raise RuntimeError("cumulative_transience module could not be initialized.")
        analysis_res = ct.analyze_audio(y.astype(np.float32), int(sr))

    if not analysis_res or 'rolling_prominences' not in analysis_res:
        raise ValueError("Transience analysis failed or missing 'rolling_prominences' in analysis results.")

    # Stack 4-band smooth transience prominence features along feature axis: (n_frames, 4)
    prominences = analysis_res['rolling_prominences']
    features = np.column_stack(prominences)

    times = analysis_res.get('times', [])
    raw_frame_dur_ms = float((times[1] - times[0]) * 1000.0) if len(times) > 1 else 1.0

    # Downsample features to target hop_ms (~10ms per frame / 100 Hz resolution)
    step = max(1, int(round(hop_ms / raw_frame_dur_ms)))
    features = features[::step]
    frame_duration_ms = raw_frame_dur_ms * step

    # Normalize features (zero mean, unit variance per feature dimension)
    mean = np.mean(features, axis=0, keepdims=True)
    std = np.std(features, axis=0, keepdims=True) + 1e-8
    features = (features - mean) / std

    return y, sr, features, frame_duration_ms, analysis_res


def ensure_ct_initialized():
    try:
        ct_utils.ensure_extension_built()
        import cumulative_transience as ct
        return ct
    except Exception as e:
        print(f"Warning: Could not initialize cumulative_transience: {e}")
        return None


def _plot_histogram_process(raw_diffs, center_point_ms):
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


def export_interactive_html_report(audio_path, y, sr, raw_diffs, center_point_ms, best_bar_length, best_patterns):
    """
    Exports an interactive HTML report containing:
    1. Histogram of transient time differences distribution with marked cluster center.
    2. Interactive waveform graph with pattern overlays, real-time playhead cursor tracking,
       HTML5 audio playback, and click-to-seek waveform navigation.
    """
    import io
    import base64
    import json
    import webbrowser

    print("\nGenerating interactive HTML report...")

    # 1. Render Histogram Plot to Base64 PNG
    hist_b64 = ""
    try:
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(10, 4.5))
        diffs_arr = np.array(raw_diffs, dtype=np.float64)
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

        buf = io.BytesIO()
        plt.savefig(buf, format='png', dpi=120)
        plt.close(fig)
        buf.seek(0)
        hist_b64 = base64.b64encode(buf.read()).decode('utf-8')
    except Exception as e:
        print(f"Could not generate histogram image for HTML report: {e}")

    # 2. Downsample Audio Waveform for Canvas Rendering (e.g. 1500 points)
    num_waveform_pts = 1500
    if len(y) > num_waveform_pts:
        step = len(y) // num_waveform_pts
        waveform_min = [float(np.min(y[i*step:(i+1)*step])) for i in range(num_waveform_pts)]
        waveform_max = [float(np.max(y[i*step:(i+1)*step])) for i in range(num_waveform_pts)]
    else:
        waveform_min = [float(val) for val in y]
        waveform_max = [float(val) for val in y]

    total_dur_s = float(len(y)) / float(sr)

    # 3. Read audio file to Base64 for embedded HTML playback
    audio_b64 = ""
    audio_mime = "audio/wav"
    if audio_path.lower().endswith(".mp3"):
        audio_mime = "audio/mp3"
    elif audio_path.lower().endswith(".ogg"):
        audio_mime = "audio/ogg"
    elif audio_path.lower().endswith(".flac"):
        audio_mime = "audio/flac"

    try:
        with open(audio_path, "rb") as af:
            audio_b64 = base64.b64encode(af.read()).decode("utf-8")
    except Exception as e:
        print(f"Could not embed base64 audio in HTML: {e}")

    audio_src = f"data:{audio_mime};base64,{audio_b64}" if audio_b64 else os.path.basename(audio_path)

    # Convert pattern data for JS consumption
    patterns_js = json.dumps(best_patterns)

    audio_filename = os.path.basename(audio_path)
    html_filepath = os.path.splitext(audio_path)[0] + "_pattern_analysis.html"

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Pattern Analysis Report - {audio_filename}</title>
    <style>
        body {{
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background-color: #f8f9fa;
            color: #2c3e50;
            margin: 0;
            padding: 20px;
        }}
        .container {{
            max-width: 1200px;
            margin: 0 auto;
            background: #ffffff;
            padding: 25px;
            border-radius: 10px;
            box-shadow: 0 4px 15px rgba(0,0,0,0.1);
        }}
        h1 {{
            color: #2c3e50;
            border-bottom: 2px solid #ecf0f1;
            padding-bottom: 10px;
            margin-top: 0;
        }}
        .metrics-card {{
            display: flex;
            gap: 20px;
            margin-bottom: 25px;
        }}
        .metric-box {{
            flex: 1;
            background: #eef2f7;
            padding: 15px;
            border-radius: 8px;
            text-align: center;
            border-left: 4px solid #3498db;
        }}
        .metric-value {{
            font-size: 22px;
            font-weight: bold;
            color: #2980b9;
        }}
        .metric-label {{
            font-size: 13px;
            color: #7f8c8d;
            text-transform: uppercase;
        }}
        .section-title {{
            font-size: 18px;
            font-weight: bold;
            margin-top: 25px;
            margin-bottom: 15px;
            color: #34495e;
        }}
        .img-container {{
            text-align: center;
            margin-bottom: 25px;
        }}
        .img-container img {{
            max-width: 100%;
            height: auto;
            border-radius: 8px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.08);
        }}
        .audio-controls {{
            margin: 20px 0;
            text-align: center;
        }}
        audio {{
            width: 100%;
            max-width: 800px;
            outline: none;
        }}
        .canvas-container {{
            position: relative;
            margin-top: 15px;
            border: 1px solid #dcdde1;
            border-radius: 8px;
            overflow: hidden;
            background: #ffffff;
            cursor: pointer;
        }}
        canvas {{
            display: block;
            width: 100%;
            height: 300px;
        }}
        .hint {{
            font-size: 12px;
            color: #7f8c8d;
            text-align: center;
            margin-top: 6px;
        }}
    </style>
</head>
<body>
<div class="container">
    <h1>Audio Pattern Analysis Report</h1>
    <div style="font-size: 14px; color: #7f8c8d; margin-bottom: 20px;">
        File: <strong>{audio_filename}</strong> | Total Duration: <strong>{total_dur_s:.2f}s</strong>
    </div>

    <div class="metrics-card">
        <div class="metric-box">
            <div class="metric-value">{best_bar_length:.2f} ms</div>
            <div class="metric-label">Determined Bar Length</div>
        </div>
        <div class="metric-box">
            <div class="metric-value">{center_point_ms:.2f} ms</div>
            <div class="metric-label">Transient Cluster Center</div>
        </div>
        <div class="metric-box">
            <div class="metric-value">{len(best_patterns)}</div>
            <div class="metric-label">Patterns Identified</div>
        </div>
    </div>

    <div class="section-title">1. Transient Pairwise Time Differences Distribution</div>
    <div class="img-container">
        <img src="data:image/png;base64,{hist_b64}" alt="Histogram Plot">
    </div>

    <div class="section-title">2. Interactive Audio Waveform & Pattern Map</div>
    <div class="audio-controls">
        <audio id="audioPlayer" controls src="{audio_src}"></audio>
    </div>

    <div class="canvas-container">
        <canvas id="waveformCanvas" width="1150" height="300"></canvas>
    </div>
    <div class="hint">💡 Click anywhere on the waveform graph above to seek to that point in the song and play audio.</div>
</div>

<script>
    const waveformMin = {json.dumps(waveform_min)};
    const waveformMax = {json.dumps(waveform_max)};
    const totalDurationS = {total_dur_s};
    const bestBarLengthMs = {best_bar_length};
    const patterns = {patterns_js};

    const canvas = document.getElementById('waveformCanvas');
    const ctx = canvas.getContext('2d');
    const audio = document.getElementById('audioPlayer');

    const colors = ['rgba(46, 204, 113, 0.35)', 'rgba(231, 76, 60, 0.35)', 'rgba(155, 89, 182, 0.35)',
                    'rgba(241, 196, 15, 0.35)', 'rgba(26, 188, 156, 0.35)', 'rgba(230, 126, 34, 0.35)'];
    const borderColors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22'];

    function draw() {{
        const W = canvas.width;
        const H = canvas.height;
        ctx.clearRect(0, 0, W, H);

        // Background
        ctx.fillStyle = '#f8f9fa';
        ctx.fillRect(0, 0, W, H);

        // Pattern Highlight Spans
        patterns.forEach((pat, idx) => {{
            const color = colors[idx % colors.length];
            const borderColor = borderColors[idx % borderColors.length];

            const startX = (pat.start_ms / 1000.0 / totalDurationS) * W;
            const endX = (pat.end_ms / 1000.0 / totalDurationS) * W;
            const spanW = endX - startX;

            ctx.fillStyle = color;
            ctx.fillRect(startX, 0, spanW, H);

            // Pattern Banner
            ctx.fillStyle = borderColor;
            ctx.font = 'bold 12px Segoe UI, sans-serif';
            ctx.fillText(`Pattern ${{idx + 1}} (${{Math.round(pat.duration_ms)}}ms)`, startX + 5, 20);

            // Segment Dividers
            if (pat.segments) {{
                pat.segments.forEach((segIdx) => {{
                    const segStartMs = segIdx * bestBarLengthMs;
                    const segStartX = (segStartMs / 1000.0 / totalDurationS) * W;

                    ctx.strokeStyle = borderColor;
                    ctx.setLineDash([4, 4]);
                    ctx.beginPath();
                    ctx.moveTo(segStartX, 25);
                    ctx.lineTo(segStartX, H);
                    ctx.stroke();
                    ctx.setLineDash([]);

                    ctx.fillStyle = borderColor;
                    ctx.font = '10px Segoe UI, sans-serif';
                    ctx.fillText(`Seg ${{segIdx}}`, segStartX + 3, 38);
                }});
            }}
        }});

        // Waveform
        const centerY = H / 2;
        const numPts = waveformMin.length;
        ctx.lineWidth = 1.2;
        ctx.strokeStyle = '#2c3e50';

        ctx.beginPath();
        for (let i = 0; i < numPts; i++) {{
            const x = (i / numPts) * W;
            const minY = centerY - (waveformMin[i] * (H * 0.4));
            const maxY = centerY - (waveformMax[i] * (H * 0.4));

            ctx.moveTo(x, minY);
            ctx.lineTo(x, maxY);
        }}
        ctx.stroke();

        // Center Axis Line
        ctx.strokeStyle = 'rgba(127, 140, 141, 0.3)';
        ctx.beginPath();
        ctx.moveTo(0, centerY);
        ctx.lineTo(W, centerY);
        ctx.stroke();

        // Playhead Cursor
        if (audio.duration) {{
            const progress = audio.currentTime / audio.duration;
            const cursorX = progress * W;

            ctx.strokeStyle = '#e67e22';
            ctx.lineWidth = 2.5;
            ctx.beginPath();
            ctx.moveTo(cursorX, 0);
            ctx.lineTo(cursorX, H);
            ctx.stroke();

            // Cursor Time Badge
            const curTimeS = audio.currentTime.toFixed(2);
            ctx.fillStyle = '#e67e22';
            ctx.fillRect(cursorX + 2, H - 25, 60, 20);
            ctx.fillStyle = '#ffffff';
            ctx.font = 'bold 11px Segoe UI, sans-serif';
            ctx.fillText(`${{curTimeS}}s`, cursorX + 8, H - 11);
        }}
    }}

    draw();

    audio.addEventListener('timeupdate', draw);
    audio.addEventListener('play', draw);
    audio.addEventListener('pause', draw);

    canvas.addEventListener('click', (e) => {{
        const rect = canvas.getBoundingClientRect();
        const clickX = e.clientX - rect.left;
        const clickFraction = clickX / rect.width;

        if (audio.duration) {{
            audio.currentTime = clickFraction * audio.duration;
            audio.play();
        }}
    }});
</script>
</body>
</html>
"""

    with open(html_filepath, "w", encoding="utf-8") as f:
        f.write(html_content)

    print(f"Interactive HTML report generated successfully: {html_filepath}")

    try:
        webbrowser.open(os.path.abspath(html_filepath))
    except Exception as e:
        print(f"Could not open browser automatically: {e}")

    return html_filepath


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


def detect_transient_candidate_lengths(y, sr, min_seg_ms=MIN_SEGMENT_LEN_MS, atom_ms=ATOM_ITERATION_MS, max_seg_ms=None, gui_mode=True, analysis_res=None):
    """
    Detects transients across the audio file using cumulative_transience peak detection,
    computes full floating-point pairwise time differences between all transients,
    plots them on a pop-up scatter plot, marks the center point of the most common cluster,
    and returns a list containing that single cluster center segment length.
    """
    print("Detecting transients across audio file using cumulative_transience peak detection...")
    onset_times_ms = []

    if analysis_res is None:
        ct = ensure_ct_initialized()
        if ct is not None:
            try:
                analysis_res = ct.analyze_audio(y.astype(np.float32), int(sr))
            except Exception as e:
                print(f"Error running cumulative_transience peak detection: {e}")
                analysis_res = None

    if analysis_res and 'peaks' in analysis_res:
        for band_peaks in analysis_res['peaks']:
            for p in band_peaks:
                onset_times_ms.append(p['time'] * 1000.0)
        onset_times_ms = sorted(onset_times_ms)

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

    # Graph all floating-point differences on a histogram (bar graph) with marked cluster center in a separate process
    if gui_mode:
        try:
            import multiprocessing
            p = multiprocessing.Process(
                target=_plot_histogram_process,
                args=(raw_diffs, center_point_ms),
                daemon=True
            )
            p.start()
        except Exception as e:
            print(f"Could not display histogram plot GUI process: {e}")

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
    try:
        from tqdm import tqdm
    except ImportError:
        def tqdm(iterable=None, total=None, desc="", **kwargs):
            class DummyPbar:
                def update(self, n=1): pass
                def close(self): pass
            return DummyPbar()

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

    total_comparisons = (num_segments * (num_segments - 1)) // 2
    pbar = tqdm(
        total=total_comparisons,
        desc=f"DTW Comparing Segments ({seg_len_ms:.0f}ms)",
        unit="pair",
        dynamic_ncols=True,
        ascii=os.name == 'nt'
    )

    for i in range(num_segments):
        for j in range(i + 1, num_segments):
            sim = compute_segment_dtw_similarity(segments[i]['features'], segments[j]['features'])

            if gui_callback:
                gui_callback(seg_len_ms, segments[i], segments[j], sim)

            if sim >= similarity_threshold:
                matched_segment_indices.add(i)
                matched_segment_indices.add(j)
                matches.append((i, j, sim))

            pbar.update(1)

    pbar.close()

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

    # Extract frame features based on smooth transience prominence
    y, sr, features, frame_dur_ms, analysis_res = extract_frame_features(raw_y_mono, orig_sr)
    total_duration_ms = (len(y) / sr) * 1000.0

    print(f"Audio duration: {total_duration_ms:.2f} ms ({total_duration_ms/1000.0:.2f} s)")

    # Conduct transient detection to find candidate segment length (center of primary cluster)
    segment_lengths = detect_transient_candidate_lengths(
        y=y,
        sr=sr,
        min_seg_ms=min_segment_ms,
        atom_ms=atom_iteration_ms,
        max_seg_ms=max_len_ms,
        gui_mode=gui_mode,
        analysis_res=analysis_res
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

    # Graph waveform with highlighted patterns and segment boundaries & Export Interactive HTML Report
    if gui_mode and best_patterns:
        try:
            # Export interactive HTML report with audio player, playhead tracking & click-to-seek waveform
            raw_diffs = segment_lengths if len(segment_lengths) > 0 else []
            export_interactive_html_report(
                audio_path=audio_path,
                y=y,
                sr=sr,
                raw_diffs=raw_diffs,
                center_point_ms=best_bar_length,
                best_bar_length=best_bar_length,
                best_patterns=best_patterns
            )
        except Exception as e:
            print(f"Could not generate interactive HTML report: {e}")

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

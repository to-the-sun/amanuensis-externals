import os
import sys
import argparse
import traceback
import numpy as np
import soundfile as sf

try:
    import ct_utils
except ImportError:
    sys.path.append(os.path.dirname(os.path.abspath(__file__)))
    import ct_utils

from offline_transience import compute_offline_segment_transience

# Global defaults
MIN_SEGMENT_LEN_MS = 100
ATOM_ITERATION_MS = 50


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


def export_interactive_html_report(audio_path, y, sr, raw_diffs, center_point_ms, best_bar_length, best_patterns, segments_transience_data, open_browser=True):
    """
    Exports an interactive HTML report containing:
    1. Histogram of transient time differences distribution with marked cluster center.
    2. Interactive waveform graph with pattern overlays, real-time playhead cursor tracking,
       HTML5 audio playback, and click-to-seek waveform navigation.
    3. Zoomed-In Segment Inspector displaying stacked vertical views for Previous (k-1),
       Current (k), and Next (k+1) segments with transient peaks, individual peak scores,
       and segment average ratings.
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

    # Clean JSON serialization helper
    def clean_json(obj):
        if isinstance(obj, np.ndarray):
            return obj.tolist()
        if isinstance(obj, (np.float32, np.float64)):
            return float(obj)
        if isinstance(obj, (np.int32, np.int64)):
            return int(obj)
        if isinstance(obj, dict):
            return {k: clean_json(v) for k, v in obj.items() if k != 'snapshot'}
        if isinstance(obj, list):
            return [clean_json(i) for i in obj]
        return obj

    patterns_js = json.dumps(clean_json(best_patterns))
    segments_js = json.dumps(clean_json(segments_transience_data))

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
        }}
        #waveformCanvas {{
            height: 250px;
        }}
        .hint {{
            font-size: 12px;
            color: #7f8c8d;
            text-align: center;
            margin-top: 6px;
        }}
        .segment-inspector-container {{
            display: flex;
            flex-direction: column;
            gap: 15px;
            margin-top: 15px;
        }}
        .segment-box {{
            border: 1px solid #dcdde1;
            border-radius: 8px;
            padding: 12px;
            background: #fdfdfd;
            box-shadow: 0 2px 6px rgba(0,0,0,0.04);
            position: relative;
        }}
        .segment-box.active-seg-box {{
            border: 2px solid #3498db;
            background: #f4f8fc;
        }}
        .segment-box-header {{
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
        }}
        .seg-title {{
            font-size: 14px;
            font-weight: bold;
            color: #2c3e50;
        }}
        .seg-rating {{
            font-size: 14px;
            font-weight: bold;
            color: #27ae60;
            background: #e8f8f5;
            padding: 4px 10px;
            border-radius: 12px;
            border: 1px solid #2ecc71;
        }}
        .seg-canvas {{
            height: 120px;
            border: 1px solid #e1e8ed;
            border-radius: 6px;
            background: #ffffff;
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
        <canvas id="waveformCanvas" width="1150" height="250"></canvas>
    </div>
    <div class="hint">💡 Click anywhere on the waveform graph above to seek to that point in the song and play audio.</div>

    <div class="section-title">3. Zoomed-In Segment Inspector (Vertically Stacked)</div>
    <div class="segment-inspector-container">
        <div class="segment-box" id="boxPrev">
            <div class="segment-box-header">
                <span class="seg-title" id="titlePrev">Previous Segment (Seg -1)</span>
                <span class="seg-rating" id="ratingPrev">Average Rating: --</span>
            </div>
            <canvas id="canvasPrev" class="seg-canvas" width="1150" height="120"></canvas>
        </div>
        <div class="segment-box active-seg-box" id="boxCurr">
            <div class="segment-box-header">
                <span class="seg-title" id="titleCurr">Current Playing Segment (Seg 0)</span>
                <span class="seg-rating" id="ratingCurr">Average Rating: --</span>
            </div>
            <canvas id="canvasCurr" class="seg-canvas" width="1150" height="120"></canvas>
        </div>
        <div class="segment-box" id="boxNext">
            <div class="segment-box-header">
                <span class="seg-title" id="titleNext">Next Segment (Seg 1)</span>
                <span class="seg-rating" id="ratingNext">Average Rating: --</span>
            </div>
            <canvas id="canvasNext" class="seg-canvas" width="1150" height="120"></canvas>
        </div>
    </div>
</div>

<script>
    const waveformMin = {json.dumps(waveform_min)};
    const waveformMax = {json.dumps(waveform_max)};
    const totalDurationS = {total_dur_s};
    const bestBarLengthMs = {best_bar_length};
    const patterns = {patterns_js};
    const segmentsData = {segments_js};

    const canvas = document.getElementById('waveformCanvas');
    const ctx = canvas.getContext('2d');
    const audio = document.getElementById('audioPlayer');

    const colors = ['rgba(46, 204, 113, 0.35)', 'rgba(231, 76, 60, 0.35)', 'rgba(155, 89, 182, 0.35)',
                    'rgba(241, 196, 15, 0.35)', 'rgba(26, 188, 156, 0.35)', 'rgba(230, 126, 34, 0.35)'];
    const borderColors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22'];
    const bandColors = ['#e74c3c', '#3498db', '#2ecc71', '#f1c40f'];

    function drawWaveformMap() {{
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

    function drawSegmentBox(canvasId, titleId, ratingId, seg, labelPrefix, currentAudioTimeMs) {{
        const c = document.getElementById(canvasId);
        const t = document.getElementById(titleId);
        const r = document.getElementById(ratingId);
        const sCtx = c.getContext('2d');
        const W = c.width;
        const H = c.height;

        sCtx.clearRect(0, 0, W, H);

        if (!seg) {{
            t.textContent = `${{labelPrefix}} Segment (Out of Range)`;
            r.textContent = `Average Rating: N/A`;

            sCtx.fillStyle = '#f8f9fa';
            sCtx.fillRect(0, 0, W, H);
            sCtx.fillStyle = '#bdc3c7';
            sCtx.font = '14px Segoe UI, sans-serif';
            sCtx.textAlign = 'center';
            sCtx.fillText('No Segment Data', W / 2, H / 2);
            return;
        }}

        t.textContent = `${{labelPrefix}} Segment (Seg ${{seg.segment_index}} : ${{Math.round(seg.start_ms)}} - ${{Math.round(seg.end_ms)}} ms)`;
        r.textContent = `Average Rating: ${{seg.rating.toFixed(4)}}`;

        // Background
        sCtx.fillStyle = '#ffffff';
        sCtx.fillRect(0, 0, W, H);

        // Center Axis
        const centerY = H / 2;
        sCtx.strokeStyle = 'rgba(189, 195, 199, 0.4)';
        sCtx.beginPath();
        sCtx.moveTo(0, centerY);
        sCtx.lineTo(W, centerY);
        sCtx.stroke();

        // Waveform
        if (seg.waveform_min && seg.waveform_min.length > 0) {{
            const numPts = seg.waveform_min.length;
            sCtx.lineWidth = 1.0;
            sCtx.strokeStyle = '#34495e';
            sCtx.beginPath();
            for (let i = 0; i < numPts; i++) {{
                const x = (i / numPts) * W;
                const minY = centerY - (seg.waveform_min[i] * (H * 0.38));
                const maxY = centerY - (seg.waveform_max[i] * (H * 0.38));
                sCtx.moveTo(x, minY);
                sCtx.lineTo(x, maxY);
            }}
            sCtx.stroke();
        }}

        const segDurMs = seg.end_ms - seg.start_ms;

        // Draw Peaks, Peak Marker Lines, and Peak Scores
        if (seg.peaks && seg.peaks.length > 0) {{
            seg.peaks.forEach((p, pIdx) => {{
                const relMs = p.time_ms - seg.start_ms;
                const x = (relMs / segDurMs) * W;
                const color = bandColors[p.band_idx % bandColors.length];

                // Peak line
                sCtx.strokeStyle = color;
                sCtx.lineWidth = 1.8;
                sCtx.beginPath();
                sCtx.moveTo(x, 0);
                sCtx.lineTo(x, H);
                sCtx.stroke();

                // Peak Dot
                sCtx.fillStyle = color;
                sCtx.beginPath();
                sCtx.arc(x, centerY, 4, 0, 2 * Math.PI);
                sCtx.fill();

                // Peak Score Label
                const scoreStr = `Score: ${{p.total_score.toFixed(3)}}`;
                sCtx.font = 'bold 10px Segoe UI, sans-serif';
                sCtx.fillStyle = color;
                sCtx.textAlign = (x > W - 70) ? 'right' : 'left';
                const labelX = (x > W - 70) ? x - 6 : x + 6;
                const labelY = 15 + (pIdx % 3) * 16;
                sCtx.fillText(scoreStr, labelX, labelY);
            }});
        }}

        // Draw Playhead Cursor inside current playing segment
        if (currentAudioTimeMs >= seg.start_ms && currentAudioTimeMs <= seg.end_ms) {{
            const relMs = currentAudioTimeMs - seg.start_ms;
            const cursorX = (relMs / segDurMs) * W;

            sCtx.strokeStyle = '#e67e22';
            sCtx.lineWidth = 2.5;
            sCtx.beginPath();
            sCtx.moveTo(cursorX, 0);
            sCtx.lineTo(cursorX, H);
            sCtx.stroke();
        }}
    }}

    function updateSegmentInspector() {{
        const curTimeMs = (audio.currentTime || 0) * 1000.0;
        const curSegIdx = Math.floor(curTimeMs / bestBarLengthMs);

        const prevSeg = segmentsData.find(s => s.segment_index === curSegIdx - 1);
        const currSeg = segmentsData.find(s => s.segment_index === curSegIdx);
        const nextSeg = segmentsData.find(s => s.segment_index === curSegIdx + 1);

        drawSegmentBox('canvasPrev', 'titlePrev', 'ratingPrev', prevSeg, 'Previous', curTimeMs);
        drawSegmentBox('canvasCurr', 'titleCurr', 'ratingCurr', currSeg, 'Current Playing', curTimeMs);
        drawSegmentBox('canvasNext', 'titleNext', 'ratingNext', nextSeg, 'Next', curTimeMs);
    }}

    function renderAll() {{
        drawWaveformMap();
        updateSegmentInspector();
    }}

    renderAll();

    audio.addEventListener('timeupdate', renderAll);
    audio.addEventListener('play', renderAll);
    audio.addEventListener('pause', renderAll);

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

    if open_browser:
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


def analyze_segment_length(y, sr, seg_len_ms, total_duration_ms, analysis_res=None):
    """
    Divides audio into contiguous segments of seg_len_ms, computes segment average ratings
    via cumulative_transience, and evaluates pattern spans based on segment ratings.
    Returns (patterns, total_pattern_duration_ms, segments_transience_data)
    """
    all_peaks_flat = []
    if analysis_res and 'peaks' in analysis_res:
        for band_idx, band_peaks in enumerate(analysis_res['peaks']):
            for p in band_peaks:
                p_copy = dict(p)
                p_copy['band_idx'] = band_idx
                all_peaks_flat.append(p_copy)

    # Compute segment transience ratings
    segments_transience_data = compute_offline_segment_transience(
        all_peaks_flat=all_peaks_flat,
        total_duration_ms=total_duration_ms,
        bar_length_ms=seg_len_ms
    )

    num_segments = len(segments_transience_data)
    if num_segments < 2:
        return [], 0.0, segments_transience_data

    patterns = []
    total_pattern_duration_ms = 0.0

    i = 0
    while i < num_segments:
        current_pattern = [segments_transience_data[i]]
        lowest_rating = segments_transience_data[i]['rating']

        j = i + 1
        while j < num_segments:
            cand_seg = segments_transience_data[j]
            rating_without = lowest_rating * len(current_pattern)
            new_lowest = min(lowest_rating, cand_seg['rating'])
            rating_with = new_lowest * (len(current_pattern) + 1)
            individual_rating = cand_seg['rating']

            # Inclusion criteria:
            # 1. Including cand_seg must not decrease overall pattern rating (rating_with >= rating_without)
            # 2. Individual rating on its own must not be higher than pattern rating if included (individual_rating <= rating_with)
            if rating_with >= rating_without and individual_rating <= rating_with:
                current_pattern.append(cand_seg)
                lowest_rating = new_lowest
                j += 1
            else:
                break

        if len(current_pattern) >= 2:
            start_ms = current_pattern[0]['start_ms']
            end_ms = current_pattern[-1]['end_ms']
            duration_ms = end_ms - start_ms
            seg_indices = [s['segment_index'] for s in current_pattern]
            pattern_rating = lowest_rating * len(current_pattern)

            patterns.append({
                'start_ms': start_ms,
                'end_ms': end_ms,
                'duration_ms': duration_ms,
                'rating': pattern_rating,
                'segments': seg_indices
            })
            total_pattern_duration_ms += duration_ms
            i = j
        else:
            i += 1

    return patterns, total_pattern_duration_ms, segments_transience_data


def find_patterns(audio_path, min_segment_ms=MIN_SEGMENT_LEN_MS, atom_iteration_ms=ATOM_ITERATION_MS, max_len_ms=None, gui_mode=True):
    """
    Full pipeline to search for patterns across transient-derived segment lengths,
    determine optimal bar length, and export interactive HTML report.
    """
    if not os.path.exists(audio_path):
        raise FileNotFoundError(f"Audio file not found: {audio_path}")

    print(f"Loading audio file: {audio_path}")
    raw_y, orig_sr = sf.read(audio_path)
    if raw_y.ndim > 1:
        raw_y_mono = np.mean(raw_y, axis=1)
    else:
        raw_y_mono = raw_y

    ct = ensure_ct_initialized()
    if ct is None:
        raise RuntimeError("cumulative_transience module could not be initialized.")
    analysis_res = ct.analyze_audio(raw_y_mono.astype(np.float32), int(orig_sr))

    y = raw_y_mono
    sr = orig_sr
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
    best_segments_transience_data = []
    results = {}

    for seg_len in segment_lengths:
        print(f"\nAnalyzing target segment length: {seg_len:.2f} ms...")
        patterns, total_pat_len_ms, seg_trans_data = analyze_segment_length(
            y=y,
            sr=sr,
            seg_len_ms=seg_len,
            total_duration_ms=total_duration_ms,
            analysis_res=analysis_res
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
            best_segments_transience_data = seg_trans_data

    if best_bar_length is None or not best_patterns:
        print("\nNo repeating patterns were detected across tested segment lengths.")
        return

    print("\n" + "="*60)
    print(f"ANALYSIS COMPLETE: Bar length determined to be {best_bar_length:.2f} ms")
    print(f"Total pattern duration at bar length: {max_total_pattern_len_ms:.2f} ms")
    print(f"Number of patterns identified: {len(best_patterns)}")
    print("="*60)

    segments_transience_data = best_segments_transience_data

    # Downsample segment audio waveforms for zoomed-in rendering
    waveform_pts_per_seg = 300
    for seg in segments_transience_data:
        s_sample = max(0, int(round((seg['start_ms'] / 1000.0) * sr)))
        e_sample = min(len(y), int(round((seg['end_ms'] / 1000.0) * sr)))
        seg_y = y[s_sample:e_sample]

        if len(seg_y) > 0:
            step = max(1, len(seg_y) // waveform_pts_per_seg)
            seg['waveform_min'] = [float(np.min(seg_y[i*step:(i+1)*step])) for i in range(len(seg_y)//step)]
            seg['waveform_max'] = [float(np.max(seg_y[i*step:(i+1)*step])) for i in range(len(seg_y)//step)]
        else:
            seg['waveform_min'] = []
            seg['waveform_max'] = []

    # Export Interactive HTML Report & Graph Waveform with highlighted patterns
    if best_patterns:
        try:
            # Export interactive HTML report with audio player, playhead tracking, click-to-seek waveform & segment inspector
            raw_diffs = segment_lengths if len(segment_lengths) > 0 else []
            export_interactive_html_report(
                audio_path=audio_path,
                y=y,
                sr=sr,
                raw_diffs=raw_diffs,
                center_point_ms=best_bar_length,
                best_bar_length=best_bar_length,
                best_patterns=best_patterns,
                segments_transience_data=segments_transience_data,
                open_browser=gui_mode
            )
        except Exception as e:
            print(f"Could not generate interactive HTML report: {e}")

    if gui_mode and best_patterns:
        try:
            import matplotlib.pyplot as plt

            fig, ax = plt.subplots(figsize=(12, 6))
            time_axis_s = np.linspace(0, total_duration_ms / 1000.0, len(y))
            ax.plot(time_axis_s, y, color='#2c3e50', alpha=0.6, linewidth=0.8, label='Waveform')

            colors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22', '#3498db']
            y_max = float(np.max(y)) if len(y) > 0 else 1.0

            for pat_idx, pat in enumerate(best_patterns, 1):
                color = colors[(pat_idx - 1) % len(colors)]
                pat_start_s = pat['start_ms'] / 1000.0
                pat_end_s = pat['end_ms'] / 1000.0

                # Highlight pattern span
                ax.axvspan(pat_start_s, pat_end_s, color=color, alpha=0.35,
                           label=f"Pattern {pat_idx} ({pat['duration_ms']:.0f} ms)")

                # Mark constituent segments
                for seg_i, seg_idx in enumerate(pat['segments']):
                    s_start_ms = seg_idx * best_bar_length
                    s_end_ms = (seg_idx + 1) * best_bar_length
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
    parser = argparse.ArgumentParser(description="Find audio patterns and bar length using cumulative transience rating span evaluation.")
    parser.add_argument("audio_file", nargs="?", help="Path to input audio file.")
    parser.add_argument("--min-segment", type=int, default=MIN_SEGMENT_LEN_MS, help="Minimum segment length in ms (default: 100ms).")
    parser.add_argument("--atom-iteration", type=int, default=ATOM_ITERATION_MS, help="Atom of iteration in ms (default: 50ms).")
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

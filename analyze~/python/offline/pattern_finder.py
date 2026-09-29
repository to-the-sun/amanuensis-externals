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


def export_interactive_html_report(audio_path, y, sr, hp_changes, best_bar_length, best_patterns, segments_transience_data, open_browser=True, pass2_res=None):
    """
    Exports an interactive HTML report containing:
    1. Cumulative history graph high point analysis summary.
    2. Interactive waveform graph with pattern overlays, marked cumulative history graph
       high point change markers, real-time playhead cursor tracking, HTML5 audio playback,
       and click-to-seek waveform navigation.
    3. Zoomed-In Segment Inspector displaying stacked vertical views for Previous (k-1),
       Current (k), and Next (k+1) segments with transient peaks, individual peak scores,
       and segment average ratings.
    4. Real-Time Accumulated History Buffer Graph updating in sync with audio playhead.
    """
    import base64
    import json
    import webbrowser

    print("\nGenerating interactive HTML report...")

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
            return {k: clean_json(v) for k, v in obj.items()}
        if isinstance(obj, list):
            return [clean_json(i) for i in obj]
        return obj

    pass2_win_ms = int(round(best_bar_length * 2))
    tolerance_ms = float(pass2_res.get('tolerance', 19.0)) if (pass2_res and isinstance(pass2_res, dict)) else 19.0

    pass2_peaks_flat = []
    if pass2_res and isinstance(pass2_res, dict) and 'peaks' in pass2_res:
        for band_idx, band_peaks in enumerate(pass2_res['peaks']):
            for p in band_peaks:
                p_copy = dict(p)
                p_copy['band_idx'] = band_idx
                p_copy['time_ms'] = float(p.get('time', 0.0) * 1000.0)
                pass2_peaks_flat.append(p_copy)

    pass2_peaks_js = json.dumps(clean_json(pass2_peaks_flat))
    patterns_js = json.dumps(clean_json(best_patterns))
    segments_js = json.dumps(clean_json(segments_transience_data))
    hp_changes_js = json.dumps(clean_json(hp_changes))

    # Compute global extreme positive and negative scores across all segments
    all_scores = [p['total_score'] for seg in segments_transience_data if 'peaks' in seg for p in seg['peaks']]
    pos_scores = [s for s in all_scores if s > 0]
    neg_scores = [s for s in all_scores if s < 0]

    global_max_pos_score = float(max(pos_scores)) if pos_scores else 1.0
    global_min_neg_score = float(min(neg_scores)) if neg_scores else -1.0

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
        #historyBufferCanvas {{
            height: 220px;
            background-color: #121216;
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
            <div class="metric-label">Segment Length (Longest High Point)</div>
        </div>
        <div class="metric-box">
            <div class="metric-value">{len(hp_changes)}</div>
            <div class="metric-label">High Point Change Events</div>
        </div>
        <div class="metric-box">
            <div class="metric-value">{len(best_patterns)}</div>
            <div class="metric-label">Patterns Identified</div>
        </div>
    </div>

    <div class="section-title">1. Interactive Audio Waveform, Pattern Map & Cumulative History High Point Changes</div>
    <div class="audio-controls">
        <audio id="audioPlayer" controls src="{audio_src}"></audio>
    </div>

    <div class="canvas-container">
        <canvas id="waveformCanvas" width="1150" height="250"></canvas>
    </div>
    <div class="hint">💡 Click anywhere on the waveform graph above to seek to that point in the song and play audio. Red markers denote points where the cumulative transience history high point changed.</div>

    <div class="section-title">2. Zoomed-In Segment Inspector (Vertically Stacked)</div>
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

    <div class="section-title">3. Real-Time Accumulated History Buffer</div>
    <div class="canvas-container" style="cursor: default; background: #121216;">
        <canvas id="historyBufferCanvas" width="1150" height="220"></canvas>
    </div>
    <div class="hint">💡 Graph updates in real time during audio playback, displaying accumulated energy, score heights, qualifiers, tolerance band, and midpoint demarcation line.</div>
</div>

<script>
    const waveformMin = {json.dumps(waveform_min)};
    const waveformMax = {json.dumps(waveform_max)};
    const totalDurationS = {total_dur_s};
    const bestBarLengthMs = {best_bar_length};
    const patterns = {patterns_js};
    const segmentsData = {segments_js};
    const hpChanges = {hp_changes_js};
    const globalMaxPosScore = {global_max_pos_score};
    const globalMinNegScore = {global_min_neg_score};
    const pass2WinMs = {pass2_win_ms};
    const toleranceMs = {tolerance_ms};
    const pass2Peaks = {pass2_peaks_js};

    const canvas = document.getElementById('waveformCanvas');
    const ctx = canvas.getContext('2d');
    const audio = document.getElementById('audioPlayer');
    const bufCanvas = document.getElementById('historyBufferCanvas');
    const bufCtx = bufCanvas ? bufCanvas.getContext('2d') : null;

    const colors = ['rgba(46, 204, 113, 0.35)', 'rgba(231, 76, 60, 0.35)', 'rgba(155, 89, 182, 0.35)',
                    'rgba(241, 196, 15, 0.35)', 'rgba(26, 188, 156, 0.35)', 'rgba(230, 126, 34, 0.35)'];
    const borderColors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22'];
    const bandColors = ['#e74c3c', '#3498db', '#2ecc71', '#f1c40f'];

    function getScoreColor(score, alpha = 0.55) {{
        let norm = 0;
        if (score > 0) {{
            norm = Math.min(1.0, score / (globalMaxPosScore || 1.0));
        }} else if (score < 0) {{
            norm = -Math.min(1.0, Math.abs(score) / Math.abs(globalMinNegScore || -1.0));
        }}

        let r = 128, g = 128, b = 128;
        if (norm > 0) {{
            r = Math.round(128 + (46 - 128) * norm);
            g = Math.round(128 + (204 - 128) * norm);
            b = Math.round(128 + (113 - 128) * norm);
        }} else if (norm < 0) {{
            const absNorm = Math.abs(norm);
            r = Math.round(128 + (231 - 128) * absNorm);
            g = Math.round(128 + (76 - 128) * absNorm);
            b = Math.round(128 + (60 - 128) * absNorm);
        }}
        return `rgba(${{r}}, ${{g}}, ${{b}}, ${{alpha}})`;
    }}

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

        // High Point Change Markers
        hpChanges.forEach((ch, idx) => {{
            const x = (ch.time_s / totalDurationS) * W;

            ctx.strokeStyle = '#e74c3c';
            ctx.lineWidth = 1.5;
            ctx.setLineDash([3, 3]);
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, H);
            ctx.stroke();
            ctx.setLineDash([]);

            // Label marker
            ctx.fillStyle = '#e74c3c';
            ctx.font = 'bold 9px Segoe UI, sans-serif';
            const labelStr = `HP: ${{ch.new_value_ms.toFixed(0)}}ms`;
            const yPos = 45 + (idx % 4) * 14;
            ctx.fillText(labelStr, Math.min(x + 2, W - 60), yPos);
        }});

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

        // Draw Peaks, Peak Marker Lines, Peak Dots, and Peak Scores
        if (seg.peaks && seg.peaks.length > 0) {{
            seg.peaks.forEach((p, pIdx) => {{
                const relMs = p.time_ms - seg.start_ms;
                const x = (relMs / segDurMs) * W;
                const score = p.total_score;
                const scoreColor = getScoreColor(score, 0.55);

                // Peak line (dashed and semi-transparent)
                sCtx.strokeStyle = scoreColor;
                sCtx.lineWidth = 1.8;
                sCtx.setLineDash([4, 4]);
                sCtx.beginPath();
                sCtx.moveTo(x, 0);
                sCtx.lineTo(x, H);
                sCtx.stroke();
                sCtx.setLineDash([]);

                // Compute Y height based on scaled score
                let scoreY = centerY;
                if (score > 0) {{
                    const ratio = Math.min(1.0, score / (globalMaxPosScore || 1.0));
                    scoreY = centerY - ratio * (centerY * 0.85);
                }} else if (score < 0) {{
                    const ratio = Math.min(1.0, Math.abs(score) / Math.abs(globalMinNegScore || -1.0));
                    scoreY = centerY + ratio * ((H - centerY) * 0.85);
                }}

                // Peak Dot at Score Height
                sCtx.fillStyle = scoreColor;
                sCtx.beginPath();
                sCtx.arc(x, scoreY, 4.5, 0, 2 * Math.PI);
                sCtx.fill();

                // Peak Score Label at Score Height
                const scoreStr = `Score: ${{score.toFixed(3)}}`;
                sCtx.font = 'bold 10px Segoe UI, sans-serif';
                sCtx.fillStyle = scoreColor;
                sCtx.textAlign = (x > W - 70) ? 'right' : 'left';
                const labelX = (x > W - 70) ? x - 6 : x + 6;
                const labelY = Math.max(12, Math.min(H - 6, scoreY + 3));
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

    function drawHistoryBuffer() {{
        if (!bufCanvas || !bufCtx) return;
        const W = bufCanvas.width;
        const H = bufCanvas.height;
        bufCtx.clearRect(0, 0, W, H);

        // Dark charcoal background matching raylib_renderer / MP4
        bufCtx.fillStyle = '#121216';
        bufCtx.fillRect(0, 0, W, H);

        const curTimeMs = (audio.currentTime || 0) * 1000.0;

        const padLeft = 65;
        const padRight = 35;
        const padTop = 30;
        const padBottom = 35;

        const graphW = W - padLeft - padRight;
        const graphH = H - padTop - padBottom;

        // Find active peaks within window [curTimeMs - pass2WinMs, curTimeMs]
        const windowStartMs = curTimeMs - pass2WinMs;
        const activePeaks = pass2Peaks.filter(p => p.time_ms > windowStartMs && p.time_ms <= curTimeMs);

        // Reconstruct accumulated buffer by summing snapshots of active peaks
        const numBufPts = (activePeaks.length > 0 && activePeaks[0].snapshot) ? activePeaks[0].snapshot.length : 200;
        const accumulatedBuffer = new Float64Array(numBufPts);

        activePeaks.forEach(p => {{
            if (p.snapshot && p.snapshot.length === numBufPts) {{
                for (let i = 0; i < numBufPts; i++) {{
                    accumulatedBuffer[i] += p.snapshot[i];
                }}
            }}
        }});

        // Compute max and min energy for Y-axis autoscaling
        let cutoffIdx = numBufPts;
        if (numBufPts > 10) {{
            const fraction99ms = 99.0 / pass2WinMs;
            cutoffIdx = Math.max(1, Math.floor(numBufPts * (1.0 - fraction99ms)));
        }}

        let curMax = 0;
        let curMin = Infinity;
        for (let i = 0; i < cutoffIdx; i++) {{
            if (accumulatedBuffer[i] > curMax) curMax = accumulatedBuffer[i];
            if (accumulatedBuffer[i] < curMin) curMin = accumulatedBuffer[i];
        }}
        if (curMax <= 0) curMax = 1.0;
        if (curMin === Infinity) curMin = 0;

        const yMax = curMax * 1.1;

        // Draw Outer Border
        bufCtx.strokeStyle = '#282830';
        bufCtx.lineWidth = 1;
        bufCtx.strokeRect(padLeft, padTop, graphW, graphH);

        // Draw Vertical Grid Lines & X-Axis Time Ticks (-pass2WinMs to 0ms)
        const numTicks = 5;
        bufCtx.fillStyle = '#8c8c96';
        bufCtx.font = '11px Segoe UI, sans-serif';
        bufCtx.textAlign = 'center';

        for (let i = 0; i <= numTicks; i++) {{
            const frac = i / numTicks;
            const x = padLeft + frac * graphW;
            const msVal = -pass2WinMs + frac * pass2WinMs;

            bufCtx.strokeStyle = 'rgba(40, 40, 48, 0.8)';
            bufCtx.beginPath();
            bufCtx.moveTo(x, padTop);
            bufCtx.lineTo(x, padTop + graphH);
            bufCtx.stroke();

            const lbl = (Math.abs(msVal) < 1e-3) ? '0ms' : `${{Math.round(msVal)}}ms`;
            bufCtx.fillText(lbl, x, padTop + graphH + 18);
        }}

        // Draw Horizontal Grid Lines & Y-Axis Energy Ticks
        bufCtx.textAlign = 'right';
        for (let ratio of [0.0, 0.25, 0.5, 0.75, 1.0]) {{
            const y = padTop + graphH - ratio * graphH;
            bufCtx.strokeStyle = 'rgba(40, 40, 48, 0.8)';
            bufCtx.beginPath();
            bufCtx.moveTo(padLeft, y);
            bufCtx.lineTo(padLeft + graphW, y);
            bufCtx.stroke();

            const valStr = (yMax * ratio).toFixed(2);
            bufCtx.fillText(valStr, padLeft - 8, y + 4);
        }}

        // Horizontal Midpoint Demarcation Line
        const midpoint = (curMin + curMax) / 2.0;
        const midY = padTop + graphH - (midpoint / yMax) * graphH;
        bufCtx.strokeStyle = '#808080';
        bufCtx.lineWidth = 1.2;
        bufCtx.setLineDash([5, 5]);
        bufCtx.beginPath();
        bufCtx.moveTo(padLeft, midY);
        bufCtx.lineTo(padLeft + graphW, midY);
        bufCtx.stroke();
        bufCtx.setLineDash([]);

        // Label Midpoint Demarcation Line
        bufCtx.fillStyle = '#808080';
        bufCtx.font = '10px Segoe UI, sans-serif';
        bufCtx.textAlign = 'left';
        bufCtx.fillText(`Midpoint: ${{midpoint.toFixed(2)}}`, padLeft + 8, midY - 4);

        // Draw Accumulated History Buffer Curve (Yellow line + area fill)
        bufCtx.strokeStyle = '#f1c40f';
        bufCtx.lineWidth = 2.0;
        bufCtx.fillStyle = 'rgba(241, 196, 15, 0.12)';

        bufCtx.beginPath();
        bufCtx.moveTo(padLeft, padTop + graphH);
        for (let i = 0; i < numBufPts; i++) {{
            const frac = i / (numBufPts - 1);
            const x = padLeft + frac * graphW;
            const val = accumulatedBuffer[i];
            const y = padTop + graphH - (val / yMax) * graphH;
            if (i === 0) bufCtx.moveTo(x, y);
            else bufCtx.lineTo(x, y);
        }}
        bufCtx.stroke();

        bufCtx.lineTo(padLeft + graphW, padTop + graphH);
        bufCtx.lineTo(padLeft, padTop + graphH);
        bufCtx.closePath();
        bufCtx.fill();

        // Draw Qualifiers and Tolerance Bands for active peaks
        if (activePeaks.length > 0) {{
            const latestPeak = activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b));
            if (latestPeak && latestPeak.qualifiers) {{
                latestPeak.qualifiers.forEach(q => {{
                    const qMs = q.ms;
                    const qVal = q.val;
                    const qOrigMs = (q.orig_ms !== undefined) ? q.orig_ms : qMs;

                    const qx = padLeft + ((qMs + pass2WinMs) / pass2WinMs) * graphW;
                    const qOrigX = padLeft + ((qOrigMs + pass2WinMs) / pass2WinMs) * graphW;
                    const tolW = (toleranceMs / pass2WinMs) * graphW;

                    const scoreColor = getScoreColor(qVal, 0.85);
                    const spanColor = getScoreColor(qVal, 0.18);

                    // Tolerance Shaded Bar
                    const spanX1 = Math.max(padLeft, qOrigX - tolW);
                    const spanX2 = Math.min(padLeft + graphW, qOrigX + tolW);
                    if (spanX2 > spanX1) {{
                        bufCtx.fillStyle = spanColor;
                        bufCtx.fillRect(spanX1, padTop, spanX2 - spanX1, graphH);
                    }}

                    // Qualifier Vertical Line
                    if (qx >= padLeft && qx <= padLeft + graphW) {{
                        bufCtx.strokeStyle = scoreColor;
                        bufCtx.lineWidth = 2.0;
                        bufCtx.setLineDash([3, 3]);
                        bufCtx.beginPath();
                        bufCtx.moveTo(qx, padTop);
                        bufCtx.lineTo(qx, padTop + graphH);
                        bufCtx.stroke();
                        bufCtx.setLineDash([]);

                        // Score label at score height
                        let normScore = 0;
                        if (qVal > 0) normScore = Math.min(1.0, qVal / (globalMaxPosScore || 1.0));
                        else if (qVal < 0) normScore = -Math.min(1.0, Math.abs(qVal) / Math.abs(globalMinNegScore || -1.0));
                        const scoreY = padTop + (graphH / 2) - normScore * (graphH / 2 * 0.8);

                        bufCtx.fillStyle = scoreColor;
                        bufCtx.font = 'bold 11px Segoe UI, sans-serif';
                        bufCtx.textAlign = (qx > padLeft + graphW - 50) ? 'right' : 'left';
                        const labelX = (qx > padLeft + graphW - 50) ? qx - 5 : qx + 5;
                        bufCtx.fillText(`${{qVal >= 0 ? '+' : ''}}${{qVal.toFixed(2)}}`, labelX, Math.max(padTop + 12, Math.min(padTop + graphH - 4, scoreY)));
                    }}
                }});
            }}
        }}

        // Title
        bufCtx.fillStyle = '#ffffff';
        bufCtx.font = 'bold 13px Segoe UI, sans-serif';
        bufCtx.textAlign = 'left';
        bufCtx.fillText(`Accumulated ${{Math.round(pass2WinMs)}}ms Historical Buffer`, padLeft, padTop - 10);
    }}

    function renderAll() {{
        drawWaveformMap();
        updateSegmentInspector();
        drawHistoryBuffer();
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


def analyze_cumulative_transience_high_points(y, sr, analysis_res=None, gui_mode=True):
    """
    Performs first-pass cumulative transience analysis on the entire audio file to accumulate
    waveforms into the 15-second cumulative transience history graph.
    Tracks the x-axis value (ms) of the high point in the cumulative history graph across frames,
    determines the high point value that persisted for the longest total duration over the course
    of the audio file, and records change events where the high point value transitioned.

    When gui_mode is enabled, it identifies the longest steady span for the selected high point value
    and pops up a window displaying the cumulative history buffer at the moment just before it
    changed away to something else.

    Returns:
      longest_high_point_ms: float (the x-axis value of the high point active for the longest duration)
      hp_changes: list of dicts [{'time_s': float, 'new_value_ms': float}] recording change events
      analysis_res: dict (the result from ct.analyze_audio)
    """
    if analysis_res is None:
        ct = ensure_ct_initialized()
        if ct is not None:
            try:
                # Pass 1: 15-second (15000 ms) rolling window and cumulative history buffer
                analysis_res = ct.analyze_audio(y.astype(np.float32), int(sr), window_ms=15000)
            except Exception as e:
                print(f"Error running cumulative_transience analysis: {e}")
                analysis_res = None

    if not analysis_res or 'highest_peaks_ms' not in analysis_res or 'times' not in analysis_res:
        print("Warning: Cumulative transience analysis yielded no high point history data.")
        return float(MIN_SEGMENT_LEN_MS), [], analysis_res

    times = analysis_res['times']
    highest_peaks = analysis_res['highest_peaks_ms']

    hp_durations = {}
    hp_changes = []
    runs = []
    current_run = None
    last_val = None

    frame_dt_s = (times[1] - times[0]) if len(times) > 1 else 0.001

    for i, (t_s, raw_hp) in enumerate(zip(times, highest_peaks)):
        if raw_hp == -999.0:
            if current_run is not None:
                runs.append(current_run)
                current_run = None
            continue

        val_ms = float(abs(raw_hp))

        # Accumulate duration for this x-axis high point value
        hp_durations[val_ms] = hp_durations.get(val_ms, 0.0) + frame_dt_s

        # Record high point changes
        if last_val is None or abs(val_ms - last_val) > 1e-3:
            hp_changes.append({
                'time_s': float(t_s),
                'new_value_ms': val_ms
            })
            last_val = val_ms

        # Group contiguous steady runs
        if current_run is None or abs(val_ms - current_run['val_ms']) > 1e-3:
            if current_run is not None:
                runs.append(current_run)
            current_run = {
                'val_ms': val_ms,
                'start_idx': i,
                'end_idx': i,
                'start_time_s': float(t_s),
                'end_time_s': float(t_s),
                'duration_s': frame_dt_s
            }
        else:
            current_run['end_idx'] = i
            current_run['end_time_s'] = float(t_s)
            current_run['duration_s'] = float(t_s) - current_run['start_time_s'] + frame_dt_s

    if current_run is not None:
        runs.append(current_run)

    if not hp_durations:
        print("Warning: No valid cumulative history high points found across frames.")
        return float(MIN_SEGMENT_LEN_MS), [], analysis_res

    # Select the x-axis high point value active for the longest total duration
    longest_high_point_ms = max(hp_durations.items(), key=lambda item: item[1])[0]

    # Find the longest steady run for the selected high point value
    hp_runs = [r for r in runs if abs(r['val_ms'] - longest_high_point_ms) < 1e-3]
    longest_run = max(hp_runs, key=lambda r: r['duration_s']) if hp_runs else None

    print(f"\nFirst Pass Cumulative Transience Analysis:")
    print(f"Tracked {len(hp_changes)} high point change events across audio.")
    print(f"High point active for the longest duration: {longest_high_point_ms:.2f} ms ({hp_durations[longest_high_point_ms]:.2f} s total)")
    if longest_run:
        target_frame = (longest_run['start_idx'] + longest_run['end_idx']) // 2
        target_time_s = times[target_frame]
        print(f"Longest steady span for high point {longest_high_point_ms:.2f} ms:")
        print(f"  Duration: {longest_run['duration_s']:.2f} s (from {longest_run['start_time_s']:.2f} s to {longest_run['end_time_s']:.2f} s)")
        print(f"  Target snapshot time at steady span midpoint: {target_time_s:.2f} s (Frame {target_frame})")

    if gui_mode and longest_run:
        try:
            import matplotlib.pyplot as plt

            ct = ensure_ct_initialized()
            if ct is not None:
                a = ct.TransientAnalyzer(1.0, int(sr), window_ms=15000)
                hop = int(sr * 0.001)
                step = hop * 100
                target_sample = int(round(target_time_s * sr)) + hop

                for last_t in range(0, target_sample, step):
                    act_s = last_t - int(sr * 0.2)
                    win_s = act_s - int(sr * 15.0)
                    if win_s < 0:
                        win_s = 0
                    rem = target_sample - last_t
                    chunk_len = rem if rem < step else step
                    push_y = y[last_t : last_t + chunk_len].astype(np.float32)
                    if len(push_y) < step:
                        push_y = np.pad(push_y, (0, step - len(push_y)))
                    a.analyze_chunk(push_y, int(sr), win_s // hop, act_s // hop)

                acc_buf = a.accumulated_buffer
                frame_duration_ms = 1000.0 * hop / float(sr)
                buffer_times = (np.arange(15001) - 15000) * frame_duration_ms

                hp_idx = 15000 - int(round(longest_high_point_ms / frame_duration_ms))
                if hp_idx < 0:
                    hp_idx = 0
                if hp_idx >= 15001:
                    hp_idx = 15000
                val_at_hp = acc_buf[hp_idx]

                fig, ax = plt.subplots(figsize=(12, 6))
                ax.axvline(-longest_high_point_ms, color='#e74c3c', linestyle='--', linewidth=2.0,
                           label=f'High Point ({longest_high_point_ms:.2f} ms)', zorder=1)
                ax.plot(-longest_high_point_ms, val_at_hp, marker='o', color='#e74c3c', markersize=8, zorder=1)

                ax.plot(buffer_times, acc_buf, color='#3498db', linewidth=1.5, label='Cumulative History Buffer', zorder=2)
                ax.fill_between(buffer_times, acc_buf, color='#3498db', alpha=0.2, zorder=2)

                offset_x = -600 if -longest_high_point_ms > -7500 else 600
                ha_align = 'right' if -longest_high_point_ms > -7500 else 'left'

                ax.annotate(f'High Point: {longest_high_point_ms:.2f} ms\nEnergy: {val_at_hp:.2f}',
                            xy=(-longest_high_point_ms, val_at_hp),
                            xytext=(-longest_high_point_ms + offset_x, 0.92),
                            textcoords=('data', 'axes fraction'),
                            arrowprops=dict(facecolor='#e74c3c', shrink=0.08, width=1.5, headwidth=8),
                            fontsize=10, fontweight='bold', color='#c0392b',
                            bbox=dict(boxstyle='round,pad=0.3', facecolor='#fadbd8', edgecolor='#e74c3c', alpha=0.9),
                            ha=ha_align, va='top')

                ax.set_title(f'Cumulative History Buffer at Peak Stability Midpoint (t = {target_time_s:.2f}s)\n'
                             f'Segment Length (High Point) = {longest_high_point_ms:.2f} ms | Longest Steady Span = {longest_run["duration_s"]:.2f}s',
                             fontsize=12, fontweight='bold', pad=12)
                ax.set_xlabel('Time Relative to Peak (ms)', fontsize=10)
                ax.set_ylabel('Accumulated Energy', fontsize=10)
                ax.set_xlim(-15000, 0)
                ax.grid(True, alpha=0.3)
                ax.legend(loc='upper left')

                fig.tight_layout()
                plt.show(block=False)
                plt.pause(0.1)
        except Exception as e:
            print(f"Could not display cumulative history buffer popup window: {e}")

    return longest_high_point_ms, hp_changes, analysis_res


def analyze_segment_length(segments_transience_data):
    """
    Analyzes contiguous segments using cumulative transience segment ratings to group
    contiguous patterns.
    Pattern rating = min(segment_ratings in pattern) * number_of_segments.
    A segment is included in a potential pattern if:
      1. Its inclusion raises (or does not lower) the overall pattern rating.
      2. Its standalone rating is not higher than the resulting pattern rating (not better off on its own).
    Returns (patterns, total_pattern_duration_ms)
    """
    if len(segments_transience_data) < 2:
        return [], 0.0

    patterns = []
    total_pattern_duration_ms = 0.0

    current_pattern = [segments_transience_data[0]]

    for seg in segments_transience_data[1:]:
        # Current pattern rating
        lowest_without = min(s['rating'] for s in current_pattern)
        rating_without = lowest_without * len(current_pattern)

        # Potential pattern rating if included
        lowest_with = min(lowest_without, seg['rating'])
        rating_with = lowest_with * (len(current_pattern) + 1)

        standalone_rating = seg['rating']

        # Exclude segment if it lowers the overall pattern rating OR if it's better off on its own
        if rating_with < rating_without or standalone_rating > rating_with:
            # End current pattern if it contains 2 or more segments
            if len(current_pattern) >= 2:
                start_ms = current_pattern[0]['start_ms']
                end_ms = current_pattern[-1]['end_ms']
                duration_ms = end_ms - start_ms
                pat_rating = min(s['rating'] for s in current_pattern) * len(current_pattern)

                patterns.append({
                    'start_ms': start_ms,
                    'end_ms': end_ms,
                    'duration_ms': duration_ms,
                    'rating': pat_rating,
                    'segments': [s['segment_index'] for s in current_pattern]
                })
                total_pattern_duration_ms += duration_ms

            # Start new candidate pattern with current segment
            current_pattern = [seg]
        else:
            current_pattern.append(seg)

    # Check last remaining pattern candidate
    if len(current_pattern) >= 2:
        start_ms = current_pattern[0]['start_ms']
        end_ms = current_pattern[-1]['end_ms']
        duration_ms = end_ms - start_ms
        pat_rating = min(s['rating'] for s in current_pattern) * len(current_pattern)

        patterns.append({
            'start_ms': start_ms,
            'end_ms': end_ms,
            'duration_ms': duration_ms,
            'rating': pat_rating,
            'segments': [s['segment_index'] for s in current_pattern]
        })
        total_pattern_duration_ms += duration_ms

    return patterns, total_pattern_duration_ms


def find_patterns(audio_path, min_segment_ms=MIN_SEGMENT_LEN_MS, atom_iteration_ms=ATOM_ITERATION_MS, max_len_ms=None, gui_mode=True):
    """
    Full pipeline to search for patterns across transient-derived segment lengths,
    determine optimal bar length, and export pattern analysis reports.
    """
    if not os.path.exists(audio_path):
        raise FileNotFoundError(f"Audio file not found: {audio_path}")

    print(f"Loading audio file: {audio_path}")
    raw_y, orig_sr = sf.read(audio_path)
    if raw_y.ndim > 1:
        y = np.mean(raw_y, axis=1)
    else:
        y = raw_y
    sr = orig_sr

    total_duration_ms = (len(y) / sr) * 1000.0
    print(f"Audio duration: {total_duration_ms:.2f} ms ({total_duration_ms/1000.0:.2f} s)")

    ct = ensure_ct_initialized()
    if ct is not None:
        try:
            # First pass: 15-second (15000 ms) window for determining segment length
            analysis_res = ct.analyze_audio(y.astype(np.float32), int(sr), window_ms=15000)
        except Exception as e:
            print(f"Error running cumulative_transience peak detection: {e}")
            analysis_res = None
    else:
        analysis_res = None

    # First pass: Cumulative transience high point analysis over entire audio
    longest_hp_ms, hp_changes, analysis_res = analyze_cumulative_transience_high_points(
        y=y,
        sr=sr,
        analysis_res=analysis_res,
        gui_mode=gui_mode
    )

    best_bar_length = longest_hp_ms

    # Second pass: Find actual peaks and score them using window_ms = segment length * 2 found in Pass 1
    if ct is not None:
        try:
            pass2_win_ms = int(round(best_bar_length * 2))
            analysis_res = ct.analyze_audio(y.astype(np.float32), int(sr), window_ms=pass2_win_ms)
        except Exception as e:
            print(f"Error running Pass 2 cumulative_transience analysis: {e}")

    # Extract all peaks across bands for segment-based transience scoring
    all_peaks_flat = []
    if analysis_res and 'peaks' in analysis_res:
        for band_idx, band_peaks in enumerate(analysis_res['peaks']):
            for p in band_peaks:
                p_copy = dict(p)
                p_copy['band_idx'] = band_idx
                all_peaks_flat.append(p_copy)

    print(f"\nAnalyzing segment length determined from cumulative history high point: {best_bar_length:.2f} ms...")

    # Extract onset_envs from analysis_res if present
    onset_envs = analysis_res.get('onset_envs', None) if analysis_res else None

    # Compute offline segment transience scoring & ratings
    segments_transience_data = compute_offline_segment_transience(
        all_peaks_flat=all_peaks_flat,
        total_duration_ms=total_duration_ms,
        bar_length_ms=best_bar_length,
        onset_envs=onset_envs
    )

    best_patterns, max_total_pattern_len_ms = analyze_segment_length(segments_transience_data)

    print("\n" + "="*60)
    print(f"ANALYSIS COMPLETE: Bar length determined to be {best_bar_length:.2f} ms")
    print(f"Total pattern duration at bar length: {max_total_pattern_len_ms:.2f} ms")
    print(f"Number of patterns identified: {len(best_patterns)}")
    print("="*60)

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
            export_interactive_html_report(
                audio_path=audio_path,
                y=y,
                sr=sr,
                hp_changes=hp_changes,
                best_bar_length=best_bar_length,
                best_patterns=best_patterns,
                segments_transience_data=segments_transience_data,
                open_browser=gui_mode,
                pass2_res=analysis_res
            )
        except Exception as e:
            print(f"Could not generate interactive HTML report: {e}")
            traceback.print_exc()

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
    parser = argparse.ArgumentParser(description="Find audio patterns and bar length using cumulative transience segment ratings.")
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

import os
import sys
import json
import struct
import base64
import argparse
import traceback

try:
    import numpy as np
except ImportError as e:
    print(f"\nMissing required dependency 'numpy': {e}")
    print("Please install required dependencies using: pip install numpy soundfile")
    input("\nPress Enter to exit...")
    sys.exit(1)

try:
    import soundfile as sf
except ImportError as e:
    print(f"\nMissing required dependency 'soundfile': {e}")
    print("Please install required dependencies using: pip install soundfile")
    input("\nPress Enter to exit...")
    sys.exit(1)

import subprocess

from offline_transience import compute_offline_segment_transience

# Global defaults
MIN_SEGMENT_LEN_MS = 100
ATOM_ITERATION_MS = 50


def export_ctbin_assets(ctbin_path, output_dir):
    """
    Parses a .ctbin binary file exported by C core ct_exporter and extracts:
    - manifest.json: Header metrics, global time-series arrays, and peak offset map
    - peaks.bin: Compact binary representation of peak metadata and qualifiers
    - snapshots.bin: Chunked binary blob containing 10x downsampled peak snapshot buffers
    """
    if not os.path.exists(ctbin_path):
        return None

    os.makedirs(output_dir, exist_ok=True)

    with open(ctbin_path, "rb") as f:
        header_data = f.read(56)
        if len(header_data) < 56:
            return None

        magic, version, sr, num_frames, win_ms, tolerance, max_peak, min_score, max_score, total_peaks = struct.unpack(
            "<4sIIII4dI", header_data
        )

        if magic != b"CTBN":
            return None

        times = np.frombuffer(f.read(num_frames * 4), dtype=np.float32)
        ratings = np.frombuffer(f.read(num_frames * 8), dtype=np.float64)
        highest_peaks = np.frombuffer(f.read(num_frames * 8), dtype=np.float64)
        demarcations = np.frombuffer(f.read(num_frames * 8), dtype=np.float64)
        global_flux = np.frombuffer(f.read(num_frames * 4), dtype=np.float32)
        global_smooth = np.frombuffer(f.read(num_frames * 4), dtype=np.float32)

        snap_len = win_ms + 1
        peaks_meta = []
        snapshots_bin_path = os.path.join(output_dir, "snapshots.bin")

        with open(snapshots_bin_path, "wb") as snap_f:
            snap_offset = 0
            for _ in range(total_peaks):
                p_idx, band_idx = struct.unpack("<ii", f.read(8))
                time_s, peak_val, score, detected_val, thresh, left_min, right_min, prominence = struct.unpack(
                    "<dddddddd", f.read(64)
                )
                num_qualifiers, = struct.unpack("<i", f.read(4))

                qualifiers = []
                for _ in range(num_qualifiers):
                    q_ms, q_val, q_orig_ms = struct.unpack("<ddd", f.read(24))
                    qualifiers.append({"ms": q_ms, "val": q_val, "orig_ms": q_orig_ms})

                full_snapshot = np.frombuffer(f.read(snap_len * 8), dtype=np.float64)
                float32_snapshot = full_snapshot.astype(np.float32)

                snap_bytes = float32_snapshot.tobytes()
                snap_f.write(snap_bytes)

                peaks_meta.append({
                    "p_idx": int(p_idx),
                    "band_idx": int(band_idx),
                    "time_s": float(time_s),
                    "time_ms": float(time_s * 1000.0),
                    "peak_val": float(peak_val),
                    "total_score": float(score),
                    "detected_peak_val": float(detected_val),
                    "thresh_val": float(thresh),
                    "left_min": float(left_min),
                    "right_min": float(right_min),
                    "prominence": float(prominence),
                    "qualifiers": qualifiers,
                    "snap_offset": snap_offset,
                    "snap_len": len(float32_snapshot)
                })

                snap_offset += len(snap_bytes)

    manifest = {
        "version": int(version),
        "sample_rate": int(sr),
        "num_frames": int(num_frames),
        "window_ms": int(win_ms),
        "tolerance": float(tolerance),
        "max_peak_value": float(max_peak),
        "min_score_seen": float(min_score),
        "max_score_seen": float(max_score),
        "total_peaks": int(total_peaks),
        "highest_peaks_ms": highest_peaks.tolist(),
        "peaks": peaks_meta,
        "demarcation_lines": demarcations.tolist()
    }

    manifest_path = os.path.join(output_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as mf:
        mf.write(json.dumps(manifest))

        snapshots_js_path = os.path.join(output_dir, "snapshots.js")
        with open(snapshots_bin_path, "rb") as snap_f:
            all_snap_bytes = snap_f.read()
            b64_snap_data = base64.b64encode(all_snap_bytes).decode("ascii")
            with open(snapshots_js_path, "w", encoding="utf-8") as js_f:
                js_f.write(f"window.snapshotsBase64 = '{b64_snap_data}';\n")

    return manifest


def get_default_tolerance():
    return 9.0


def export_interactive_html_report(audio_path, y, sr, hp_changes, best_bar_length, best_patterns, segments_transience_data, open_browser=True, pass2_res=None, ctbin_manifest=None):
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

    output_dir = os.path.splitext(audio_path)[0]
    os.makedirs(output_dir, exist_ok=True)

    # 3. Use relative path to audio file on disk for lightweight HTML report (< 100 KB)
    rel_audio_path = os.path.relpath(audio_path, output_dir).replace("\\", "/")
    audio_src = rel_audio_path

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

    pass2_win_ms = min(15000, int(round(best_bar_length * 2))) if best_bar_length > 0 else 15000
    tolerance_ms = float(pass2_res.get('tolerance', get_default_tolerance())) if (pass2_res and isinstance(pass2_res, dict)) else get_default_tolerance()
    hop = int(sr * 0.001)
    frame_duration_ms = 1000.0 * float(hop) / float(sr)

    pass2_peaks_flat = []
    dem_lines = pass2_res.get('demarcation_lines', []) if (pass2_res and isinstance(pass2_res, dict)) else []

    ctbin_peaks_map = {}
    if ctbin_manifest and isinstance(ctbin_manifest, dict) and 'peaks' in ctbin_manifest:
        for p in ctbin_manifest['peaks']:
            ctbin_peaks_map[(p.get('p_idx'), p.get('band_idx'))] = p

    if ctbin_manifest and isinstance(ctbin_manifest, dict) and 'peaks' in ctbin_manifest:
        for p in ctbin_manifest['peaks']:
            if isinstance(p, dict):
                p_copy = dict(p)
                t_ms = float(p.get('time_ms', p.get('time', 0.0) * 1000.0))
                p_copy['time_ms'] = t_ms
                if len(dem_lines) > 0:
                    f_idx = min(len(dem_lines) - 1, max(0, int(round(t_ms))))
                    p_copy['demarcation_line'] = float(dem_lines[f_idx])
                p_copy.pop('snapshot', None)
                pass2_peaks_flat.append(p_copy)

    clean_segments_data = []
    for seg in segments_transience_data:
        seg_copy = dict(seg)
        if 'peaks' in seg_copy and isinstance(seg_copy['peaks'], list):
            clean_peaks = []
            for p in seg_copy['peaks']:
                pk = dict(p)
                pk.pop('snapshot', None)  # Strip snapshot list from segment peak copies
                clean_peaks.append(pk)
            seg_copy['peaks'] = clean_peaks
        clean_segments_data.append(seg_copy)

    pass2_peaks_js = json.dumps(clean_json(pass2_peaks_flat))
    patterns_js = json.dumps(clean_json(best_patterns))
    segments_js = json.dumps(clean_json(clean_segments_data))
    hp_changes_js = json.dumps(clean_json(hp_changes))

    # Compute global extreme positive and negative scores across all segments
    all_scores = [p['total_score'] for seg in segments_transience_data if 'peaks' in seg for p in seg['peaks']]
    pos_scores = [s for s in all_scores if s > 0]
    neg_scores = [s for s in all_scores if s < 0]

    global_max_pos_score = float(max(pos_scores)) if pos_scores else 1.0
    global_min_neg_score = float(min(neg_scores)) if neg_scores else -1.0

    # Write report_data.json and report_data.js to disk so HTML report reads strictly from disk assets
    report_data = {
        "pass2_win_ms": pass2_win_ms,
        "frame_duration_ms": frame_duration_ms,
        "waveform_min": waveform_min,
        "waveform_max": waveform_max,
        "total_dur_s": total_dur_s,
        "best_bar_length": best_bar_length,
        "patterns": clean_json(best_patterns),
        "segments_data": clean_json(clean_segments_data),
        "hp_changes": clean_json(hp_changes),
        "global_max_pos_score": global_max_pos_score,
        "global_min_neg_score": global_min_neg_score,
        "tolerance_ms": tolerance_ms,
        "pass2_peaks": clean_json(pass2_peaks_flat)
    }
    report_data_path = os.path.join(output_dir, "report_data.json")
    report_data_json_str = json.dumps(report_data)
    with open(report_data_path, "w", encoding="utf-8") as rdf:
        rdf.write(report_data_json_str)

    report_data_js_path = os.path.join(output_dir, "report_data.js")
    with open(report_data_js_path, "w", encoding="utf-8") as jsf:
        jsf.write(f"window.reportData = {report_data_json_str};\n")

    audio_filename = os.path.basename(audio_path)
    audio_stem = os.path.splitext(audio_filename)[0]
    html_filepath = os.path.join(output_dir, f"{audio_stem}_pattern_analysis.html")

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Pattern Analysis Report - {audio_filename}</title>
    <script src="report_data.js"></script>
    <script src="snapshots.js"></script>
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
            background-color: #ffffff;
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
            cursor: pointer;
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
    <div class="hint">💡 Click anywhere on any of the segment graphs above to jump to that exact point in the song and play audio.</div>

    <div class="section-title">3. Accumulated History Buffer</div>
    <div class="canvas-container" style="cursor: crosshair; background: #ffffff;">
        <canvas id="historyBufferCanvas" width="1150" height="220"></canvas>
    </div>
    <div class="hint">💡 Select a section to zoom in on. Right-click anywhere on the graph to zoom back out to the full duration. Graph updates in real time during audio playback.</div>
</div>

<script>
    let pass2WinMs = 15000;
    let frameDurationMs = 1.0;
    let waveformMin = [];
    let waveformMax = [];
    let totalDurationS = 0;
    let bestBarLengthMs = 1000;
    let patterns = [];
    let segmentsData = [];
    let hpChanges = [];
    let globalMaxPosScore = 1.0;
    let globalMinNegScore = -1.0;
    let toleranceMs = 9.0;
    let pass2Peaks = [];

    function applyReportData(data) {{
        if (!data) return;
        if (data.pass2_win_ms !== undefined) pass2WinMs = data.pass2_win_ms;
        if (data.frame_duration_ms !== undefined) frameDurationMs = data.frame_duration_ms;
        if (data.waveform_min) waveformMin = data.waveform_min;
        if (data.waveform_max) waveformMax = data.waveform_max;
        if (data.total_dur_s !== undefined) totalDurationS = data.total_dur_s;
        if (data.best_bar_length !== undefined) bestBarLengthMs = data.best_bar_length;
        if (data.patterns) patterns = data.patterns;
        if (data.segments_data) segmentsData = data.segments_data;
        if (data.hp_changes) hpChanges = data.hp_changes;
        if (data.global_max_pos_score !== undefined) globalMaxPosScore = data.global_max_pos_score;
        if (data.global_min_neg_score !== undefined) globalMinNegScore = data.global_min_neg_score;
        if (data.tolerance_ms !== undefined) toleranceMs = data.tolerance_ms;
        if (data.pass2_peaks) pass2Peaks = data.pass2_peaks;
        renderAll();
    }}

    // Load report data from window.reportData (loaded via report_data.js) or fetch report_data.json
    if (window.reportData) {{
        applyReportData(window.reportData);
    }} else {{
        fetch('report_data.json')
            .then(res => res.json())
            .then(data => applyReportData(data))
            .catch(e => console.log('report_data.json fetch notice:', e));
    }}

    fetch('manifest.json')
        .then(res => res.json())
        .then(m => {{
            if (m) {{
                if (m.window_ms) pass2WinMs = m.window_ms;
                if (m.tolerance) toleranceMs = m.tolerance;
                if (m.peaks && m.peaks.length > 0) pass2Peaks = m.peaks;
                renderAll();
            }}
        }})
        .catch(e => console.log('manifest.json fetch notice:', e));

    const canvas = document.getElementById('waveformCanvas');
    const ctx = canvas.getContext('2d');
    const audio = document.getElementById('audioPlayer');
    const bufCanvas = document.getElementById('historyBufferCanvas');
    const bufCtx = bufCanvas ? bufCanvas.getContext('2d') : null;

    let zoomStartMs = -pass2WinMs;
    let zoomEndMs = 0.0;
    let isDraggingBuf = false;
    let dragStartX = 0;
    let dragCurrentX = 0;

    const colors = ['rgba(46, 204, 113, 0.35)', 'rgba(231, 76, 60, 0.35)', 'rgba(155, 89, 182, 0.35)',
                    'rgba(241, 196, 15, 0.35)', 'rgba(26, 188, 156, 0.35)', 'rgba(230, 126, 34, 0.35)'];
    const borderColors = ['#2ecc71', '#e74c3c', '#9b59b6', '#f1c40f', '#1abc9c', '#e67e22'];
    const bandColors = ['#e74c3c', '#3498db', '#2ecc71', '#f1c40f'];

    function getScoreColor(score, alpha = 1.0) {{
        if (score === 0) return `rgba(128, 128, 128, ${{alpha}})`;
        let r = 128, g = 128, b = 128;
        if (score < 0) {{
            let t = globalMinNegScore < 0 ? score / globalMinNegScore : 0.0;
            t = Math.max(0.0, Math.min(1.0, t));
            r = Math.round(128 + (255 - 128) * t);
            g = Math.round(128 + (0 - 128) * t);
            b = Math.round(128 + (0 - 128) * t);
        }} else {{
            let t = globalMaxPosScore > 0 ? score / globalMaxPosScore : 0.0;
            t = Math.max(0.0, Math.min(1.0, t));
            r = Math.round(128 + (0 - 128) * t);
            g = Math.round(128 + (255 - 128) * t);
            b = Math.round(128 + (0 - 128) * t);
        }}
        return `rgba(${{r}}, ${{g}}, ${{b}}, ${{alpha}})`;
    }}

    function getQualifierColor(val, alpha = 1.0) {{
        if (val === 0) return `rgba(128, 128, 128, ${{alpha}})`;
        let r = 128, g = 128, b = 128;
        if (val < 0) {{
            let t = Math.max(0.0, Math.min(1.0, val / -1.0));
            r = Math.round(128 + (255 - 128) * t);
            g = Math.round(128 + (0 - 128) * t);
            b = Math.round(128 + (0 - 128) * t);
        }} else {{
            let t = Math.max(0.0, Math.min(1.0, val / 1.0));
            r = Math.round(128 + (0 - 128) * t);
            g = Math.round(128 + (255 - 128) * t);
            b = Math.round(128 + (0 - 128) * t);
        }}
        return `rgba(${{r}}, ${{g}}, ${{b}}, ${{alpha}})`;
    }}

    function getLatestActivePeak() {{
        const curTimeMs = (audio.currentTime || 0) * 1000.0;
        const windowStartMs = curTimeMs - pass2WinMs;
        const activePeaks = pass2Peaks.filter(p => p.time_ms > windowStartMs && p.time_ms <= curTimeMs);
        if (activePeaks.length > 0) {{
            return activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b));
        }}
        return null;
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

    function drawSegmentBox(boxId, canvasId, titleId, ratingId, seg, labelPrefix, currentAudioTimeMs) {{
        const boxElem = document.getElementById(boxId);
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

            if (boxElem) {{
                boxElem.style.borderColor = '#dcdde1';
                boxElem.style.borderWidth = '1px';
                boxElem.style.backgroundColor = '#fdfdfd';
            }}
            if (t) t.style.color = '#2c3e50';
            if (r) {{
                r.style.color = '#7f8c8d';
                r.style.borderColor = '#bdc3c7';
                r.style.backgroundColor = '#ecf0f1';
            }}

            sCtx.fillStyle = '#f8f9fa';
            sCtx.fillRect(0, 0, W, H);
            sCtx.fillStyle = '#bdc3c7';
            sCtx.font = '14px Segoe UI, sans-serif';
            sCtx.textAlign = 'center';
            sCtx.fillText('No Segment Data', W / 2, H / 2);
            return;
        }}

        const patIdx = (seg && seg.segment_index !== undefined)
            ? patterns.findIndex(p => p.segments && p.segments.includes(seg.segment_index))
            : -1;

        if (patIdx !== -1) {{
            const mainColor = borderColors[patIdx % borderColors.length];
            const bgTint = colors[patIdx % colors.length].replace('0.35', '0.12');
            const badgeBg = colors[patIdx % colors.length].replace('0.35', '0.2');

            if (boxElem) {{
                boxElem.style.borderColor = mainColor;
                boxElem.style.borderWidth = '2px';
                boxElem.style.backgroundColor = bgTint;
            }}
            if (t) t.style.color = mainColor;
            if (r) {{
                r.style.color = mainColor;
                r.style.borderColor = mainColor;
                r.style.backgroundColor = badgeBg;
            }}

            t.textContent = `${{labelPrefix}} Segment (Seg ${{seg.segment_index}} [Pattern ${{patIdx + 1}}] : ${{Math.round(seg.start_ms)}} - ${{Math.round(seg.end_ms)}} ms)`;
        }} else {{
            if (boxId === 'boxCurr') {{
                if (boxElem) {{
                    boxElem.style.borderColor = '#3498db';
                    boxElem.style.borderWidth = '2px';
                    boxElem.style.backgroundColor = '#f4f8fc';
                }}
            }} else {{
                if (boxElem) {{
                    boxElem.style.borderColor = '#dcdde1';
                    boxElem.style.borderWidth = '1px';
                    boxElem.style.backgroundColor = '#fdfdfd';
                }}
            }}
            if (t) t.style.color = '#2c3e50';
            if (r) {{
                r.style.color = '#27ae60';
                r.style.borderColor = '#2ecc71';
                r.style.backgroundColor = '#e8f8f5';
            }}

            t.textContent = `${{labelPrefix}} Segment (Seg ${{seg.segment_index}} : ${{Math.round(seg.start_ms)}} - ${{Math.round(seg.end_ms)}} ms)`;
        }}

        r.textContent = `Average Rating: ${{seg.rating.toFixed(4)}}`;

        // Background
        sCtx.fillStyle = '#ffffff';
        sCtx.fillRect(0, 0, W, H);

        const segDurMs = seg.end_ms - seg.start_ms;

        // 1. Peak Marker Lines rendered in the back behind everything else (partially transparent)
        if (seg.peaks && seg.peaks.length > 0) {{
            seg.peaks.forEach((p) => {{
                const relMs = p.time_ms - seg.start_ms;
                const x = (relMs / segDurMs) * W;
                const score = p.total_score;
                const lineColor = getScoreColor(score, 0.55);

                sCtx.strokeStyle = lineColor;
                sCtx.lineWidth = 1.8;
                sCtx.setLineDash([4, 4]);
                sCtx.beginPath();
                sCtx.moveTo(x, 0);
                sCtx.lineTo(x, H);
                sCtx.stroke();
                sCtx.setLineDash([]);
            }});
        }}

        // 2. Center Axis
        const centerY = H / 2;
        sCtx.strokeStyle = 'rgba(189, 195, 199, 0.4)';
        sCtx.beginPath();
        sCtx.moveTo(0, centerY);
        sCtx.lineTo(W, centerY);
        sCtx.stroke();

        // 3. Waveform
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

        // 4. Playhead Cursor inside current playing segment
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

        // 5. Peak Score Values rendered in front of everything else (no "Score" word, no dot, bold if currently active)
        const latestPeak = getLatestActivePeak();

        if (seg.peaks && seg.peaks.length > 0) {{
            seg.peaks.forEach((p) => {{
                const relMs = p.time_ms - seg.start_ms;
                const x = (relMs / segDurMs) * W;
                const score = p.total_score;
                const scoreColor = getScoreColor(score, 1.0);

                // Compute Y height based on scaled score
                let scoreY = centerY;
                if (score > 0) {{
                    const ratio = Math.min(1.0, score / (globalMaxPosScore || 1.0));
                    scoreY = centerY - ratio * (centerY * 0.85);
                }} else if (score < 0) {{
                    const ratio = Math.min(1.0, Math.abs(score) / Math.abs(globalMinNegScore || -1.0));
                    scoreY = centerY + ratio * ((H - centerY) * 0.85);
                }}

                const isLatest = latestPeak && Math.abs(p.time_ms - latestPeak.time_ms) < 1e-3 && (p.band_idx === undefined || p.band_idx === latestPeak.band_idx);

                const scoreStr = score.toFixed(3);
                sCtx.font = isLatest ? 'bold 11px Segoe UI, sans-serif' : '10px Segoe UI, sans-serif';
                sCtx.fillStyle = scoreColor;
                sCtx.textAlign = (x > W - 70) ? 'right' : 'left';
                const labelX = (x > W - 70) ? x - 6 : x + 6;
                const labelY = Math.max(12, Math.min(H - 6, scoreY + 3));
                sCtx.fillText(scoreStr, labelX, labelY);
            }});
        }}
    }}

    function updateSegmentInspector() {{
        const curTimeMs = (audio.currentTime || 0) * 1000.0;
        const curSegIdx = Math.floor(curTimeMs / bestBarLengthMs);

        const prevSeg = segmentsData.find(s => s.segment_index === curSegIdx - 1);
        const currSeg = segmentsData.find(s => s.segment_index === curSegIdx);
        const nextSeg = segmentsData.find(s => s.segment_index === curSegIdx + 1);

        drawSegmentBox('boxPrev', 'canvasPrev', 'titlePrev', 'ratingPrev', prevSeg, 'Previous', curTimeMs);
        drawSegmentBox('boxCurr', 'canvasCurr', 'titleCurr', 'ratingCurr', currSeg, 'Current Playing', curTimeMs);
        drawSegmentBox('boxNext', 'canvasNext', 'titleNext', 'ratingNext', nextSeg, 'Next', curTimeMs);
    }}

    function drawHistoryBuffer() {{
        if (!bufCanvas || !bufCtx) return;
        const W = bufCanvas.width;
        const H = bufCanvas.height;
        bufCtx.clearRect(0, 0, W, H);

        // White background matching color scheme of the other panels
        bufCtx.fillStyle = '#ffffff';
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

        // Get latest active peak to utilize direct accumulated buffer array from C core or snapshots.bin
        const latestPeak = (activePeaks.length > 0) ? activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b)) : null;
        let accumulatedBuffer = null;
        if (latestPeak && window.snapshotsArrayBuffer && latestPeak.snap_offset !== undefined) {{
            const floatLen = latestPeak.snap_len || (pass2WinMs + 1);
            accumulatedBuffer = new Float32Array(window.snapshotsArrayBuffer, latestPeak.snap_offset, floatLen);
        }} else if (latestPeak && latestPeak.snapshot && latestPeak.snapshot.length > 0) {{
            accumulatedBuffer = latestPeak.snapshot;
        }}
        const numBufPts = accumulatedBuffer ? accumulatedBuffer.length : (Math.round(pass2WinMs) + 1);
        if (!accumulatedBuffer) {{
            accumulatedBuffer = new Float32Array(numBufPts);
        }}

        // Bin step based on exact sample rate hop frame duration matching C core buffer_times
        const snapBinMs = frameDurationMs || ((numBufPts > 1) ? (pass2WinMs / (numBufPts - 1)) : 1.0);

        // Compute max and min energy for Y-axis autoscaling within visible zoom window, excluding the last 99ms region [-99ms, 0ms]
        let curMax = 0;
        let curMin = Infinity;

        for (let i = 0; i < numBufPts; i++) {{
            const sampleMs = (i - (numBufPts - 1)) * snapBinMs;
            if (sampleMs >= zoomStartMs && sampleMs <= zoomEndMs && sampleMs <= -99.0 + 1e-5) {{
                const val = accumulatedBuffer[i];
                if (val > curMax) curMax = val;
                if (val < curMin) curMin = val;
            }}
        }}
        if (curMax <= 0) curMax = 1.0;
        if (curMin === Infinity) curMin = 0;

        const yMax = curMax * 1.1;

        // Draw Outer Border
        bufCtx.strokeStyle = '#dcdde1';
        bufCtx.lineWidth = 1;
        bufCtx.strokeRect(padLeft, padTop, graphW, graphH);

        // Draw Vertical Grid Lines & X-Axis Time Ticks
        const numTicks = 5;
        bufCtx.fillStyle = '#7f8c8d';
        bufCtx.font = '11px Segoe UI, sans-serif';
        bufCtx.textAlign = 'center';

        const zoomSpan = zoomEndMs - zoomStartMs;

        for (let i = 0; i <= numTicks; i++) {{
            const frac = i / numTicks;
            const x = padLeft + frac * graphW;
            const msVal = zoomStartMs + frac * zoomSpan;

            bufCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)';
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
            bufCtx.strokeStyle = 'rgba(220, 221, 225, 0.8)';
            bufCtx.beginPath();
            bufCtx.moveTo(padLeft, y);
            bufCtx.lineTo(padLeft + graphW, y);
            bufCtx.stroke();

            const valStr = (yMax * ratio).toFixed(2);
            bufCtx.fillText(valStr, padLeft - 8, y + 4);
        }}

        // Horizontal Mean Demarcation Line (Directly from C core latestPeak.demarcation_line stored per peak)
        const meanVal = (latestPeak && latestPeak.demarcation_line !== undefined) ? latestPeak.demarcation_line : 0;
        const meanY = padTop + graphH - (meanVal / yMax) * graphH;

        bufCtx.strokeStyle = '#7f8c8d';
        bufCtx.lineWidth = 1.2;
        bufCtx.setLineDash([5, 5]);
        bufCtx.beginPath();
        bufCtx.moveTo(padLeft, meanY);
        bufCtx.lineTo(padLeft + graphW, meanY);
        bufCtx.stroke();
        bufCtx.setLineDash([]);

        // Clip plotting to inner graph rectangle
        bufCtx.save();
        bufCtx.beginPath();
        bufCtx.rect(padLeft, padTop, graphW, graphH);
        bufCtx.clip();

        // Draw Accumulated History Buffer Curve (Primary blue line + area fill, stopping at -99ms to exclude [-99ms, 0ms])
        bufCtx.strokeStyle = '#3498db';
        bufCtx.lineWidth = 2.0;
        bufCtx.fillStyle = 'rgba(52, 152, 219, 0.15)';

        bufCtx.beginPath();
        let firstPt = true;
        let lastDrawnMs = (0 - (numBufPts - 1)) * snapBinMs;
        for (let i = 0; i < numBufPts; i++) {{
            const sampleMs = (i - (numBufPts - 1)) * snapBinMs;
            if (sampleMs > -99.0 + 1e-5) continue; // Exclude the 99ms region [-99ms, 0ms] from visual rendering
            const x = padLeft + ((sampleMs - zoomStartMs) / zoomSpan) * graphW;
            const val = accumulatedBuffer[i];
            const y = padTop + graphH - (val / yMax) * graphH;
            if (firstPt) {{
                bufCtx.moveTo(x, y);
                firstPt = false;
            }} else {{
                bufCtx.lineTo(x, y);
            }}
            lastDrawnMs = sampleMs;
        }}
        bufCtx.stroke();

        // Fill area under curve up to -99ms
        const endX = padLeft + ((lastDrawnMs - zoomStartMs) / zoomSpan) * graphW;
        const firstSampleMs = (0 - (numBufPts - 1)) * snapBinMs;
        const startX = padLeft + ((firstSampleMs - zoomStartMs) / zoomSpan) * graphW;

        bufCtx.lineTo(endX, padTop + graphH);
        bufCtx.lineTo(startX, padTop + graphH);
        bufCtx.closePath();
        bufCtx.fill();

        // Draw Qualifiers and Tolerance Bands for active peaks
        if (activePeaks.length > 0) {{
            const latestPeak = activePeaks.reduce((a, b) => (a.time_ms > b.time_ms ? a : b));
            if (latestPeak && latestPeak.qualifiers) {{
                latestPeak.qualifiers.forEach(q => {{
                    const qMs = q.ms;
                    const qVal = q.val;
                    const qOrigMs = (q.orig_ms !== undefined) ? q.orig_ms : q.ms;

                    const qx = padLeft + ((qMs - zoomStartMs) / zoomSpan) * graphW;
                    const qOrigX = padLeft + ((qOrigMs - zoomStartMs) / zoomSpan) * graphW;
                    const tolW = (toleranceMs / zoomSpan) * graphW;

                    const scoreColor = getQualifierColor(qVal, 0.85);
                    const spanColor = getQualifierColor(qVal, 0.18);

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

                        // Score label height relative to mean demarcation line using fixed [-1.0, 1.0] range
                        let scoreY = meanY;
                        if (qVal > 0) {{
                            const norm = Math.min(1.0, qVal / 1.0);
                            scoreY = meanY - norm * (meanY - padTop);
                        }} else if (qVal < 0) {{
                            const norm = Math.min(1.0, Math.abs(qVal) / 1.0);
                            scoreY = meanY + norm * (padTop + graphH - meanY);
                        }}

                        bufCtx.fillStyle = scoreColor;
                        bufCtx.font = 'bold 11px Segoe UI, sans-serif';
                        bufCtx.textAlign = (qx > padLeft + graphW - 50) ? 'right' : 'left';
                        const labelX = (qx > padLeft + graphW - 50) ? qx - 5 : qx + 5;
                        bufCtx.fillText(`${{qVal >= 0 ? '+' : ''}}${{qVal.toFixed(2)}}`, labelX, Math.max(padTop + 12, Math.min(padTop + graphH - 4, scoreY)));
                    }}
                }});
            }}
        }}

        bufCtx.restore(); // Remove graph clip rect

        // Title
        bufCtx.fillStyle = '#2c3e50';
        bufCtx.font = 'bold 13px Segoe UI, sans-serif';
        bufCtx.textAlign = 'left';
        if (Math.abs(zoomStartMs - (-pass2WinMs)) > 1e-2 || Math.abs(zoomEndMs - 0.0) > 1e-2) {{
            bufCtx.fillText(`Accumulated Historical Buffer [Zoomed: ${{Math.round(zoomStartMs)}}ms to ${{Math.round(zoomEndMs)}}ms]`, padLeft, padTop - 10);
        }} else {{
            bufCtx.fillText(`Accumulated ${{Math.round(pass2WinMs)}}ms Historical Buffer`, padLeft, padTop - 10);
        }}

        // Resulting score of all qualifiers (latest active peak score) in top right-hand corner
        const latestActivePeak = getLatestActivePeak();
        if (latestActivePeak) {{
            const scoreVal = latestActivePeak.total_score;
            bufCtx.fillStyle = getScoreColor(scoreVal, 1.0);
            bufCtx.font = 'bold 13px Segoe UI, sans-serif';
            bufCtx.textAlign = 'right';
            bufCtx.fillText(scoreVal.toFixed(3), padLeft + graphW, padTop - 10);
        }}

        // Selection Box Overlay while dragging
        if (isDraggingBuf) {{
            const selX = Math.min(dragStartX, dragCurrentX);
            const selW = Math.abs(dragCurrentX - dragStartX);

            bufCtx.fillStyle = 'rgba(52, 152, 219, 0.25)';
            bufCtx.fillRect(selX, padTop, selW, graphH);

            bufCtx.strokeStyle = '#2980b9';
            bufCtx.lineWidth = 1.5;
            bufCtx.setLineDash([3, 3]);
            bufCtx.strokeRect(selX, padTop, selW, graphH);
            bufCtx.setLineDash([]);
        }}
    }}

    if (bufCanvas) {{
        function getCanvasMouseX(e) {{
            const rect = bufCanvas.getBoundingClientRect();
            if (!rect.width) return 0;
            const scaleX = bufCanvas.width / rect.width;
            return (e.clientX - rect.left) * scaleX;
        }}

        bufCanvas.addEventListener('contextmenu', (e) => {{
            e.preventDefault();
            zoomStartMs = -pass2WinMs;
            zoomEndMs = 0.0;
            isDraggingBuf = false;
            drawHistoryBuffer();
        }});

        bufCanvas.addEventListener('mousedown', (e) => {{
            if (e.button !== 0) return;
            const clickX = getCanvasMouseX(e);
            const padLeft = 65;
            const padRight = 35;
            const graphW = bufCanvas.width - padLeft - padRight;

            if (clickX >= padLeft && clickX <= padLeft + graphW) {{
                isDraggingBuf = true;
                dragStartX = clickX;
                dragCurrentX = clickX;
            }}
        }});

        bufCanvas.addEventListener('mousemove', (e) => {{
            if (!isDraggingBuf) return;
            const mouseX = getCanvasMouseX(e);
            const padLeft = 65;
            const padRight = 35;
            const graphW = bufCanvas.width - padLeft - padRight;

            dragCurrentX = Math.max(padLeft, Math.min(padLeft + graphW, mouseX));
            drawHistoryBuffer();
        }});

        bufCanvas.addEventListener('mouseup', (e) => {{
            if (!isDraggingBuf || e.button !== 0) return;
            isDraggingBuf = false;

            const padLeft = 65;
            const padRight = 35;
            const graphW = bufCanvas.width - padLeft - padRight;

            const dx = Math.abs(dragCurrentX - dragStartX);
            if (dx > 5) {{
                const x1 = Math.min(dragStartX, dragCurrentX);
                const x2 = Math.max(dragStartX, dragCurrentX);

                const frac1 = (x1 - padLeft) / graphW;
                const frac2 = (x2 - padLeft) / graphW;

                const currentSpan = zoomEndMs - zoomStartMs;
                const newStartMs = zoomStartMs + frac1 * currentSpan;
                const newEndMs = zoomStartMs + frac2 * currentSpan;

                if (newEndMs - newStartMs >= 10.0) {{
                    zoomStartMs = newStartMs;
                    zoomEndMs = newEndMs;
                }}
            }}
            drawHistoryBuffer();
        }});

        bufCanvas.addEventListener('mouseleave', () => {{
            if (isDraggingBuf) {{
                isDraggingBuf = false;
                drawHistoryBuffer();
            }}
        }});
    }}

    function renderAll() {{
        drawWaveformMap();
        updateSegmentInspector();
        drawHistoryBuffer();
    }}

    // Load binary snapshot chunks from snapshots.js or fallback to snapshots.bin fetch
    if (window.snapshotsBase64) {{
        try {{
            const binaryString = atob(window.snapshotsBase64);
            const len = binaryString.length;
            const bytes = new Uint8Array(len);
            for (let i = 0; i < len; i++) {{
                bytes[i] = binaryString.charCodeAt(i);
            }}
            window.snapshotsArrayBuffer = bytes.buffer;
        }} catch (e) {{
            console.log('Error decoding snapshotsBase64:', e);
        }}
    }} else {{
        fetch('snapshots.bin')
            .then(res => res.arrayBuffer())
            .then(buf => {{
                window.snapshotsArrayBuffer = buf;
                drawHistoryBuffer();
            }})
            .catch(e => console.log('snapshots.bin lazy-load fetch notice:', e));
    }}

    renderAll();

    audio.addEventListener('timeupdate', renderAll);
    audio.addEventListener('play', renderAll);
    audio.addEventListener('pause', renderAll);

    canvas.addEventListener('click', (e) => {{
        const rect = canvas.getBoundingClientRect();
        const clickX = e.clientX - rect.left;
        const clickFraction = Math.max(0, Math.min(1, clickX / rect.width));
        const dur = (audio.duration && !isNaN(audio.duration) && audio.duration > 0) ? audio.duration : totalDurationS;

        if (dur > 0) {{
            const wasPaused = audio.paused;
            audio.currentTime = clickFraction * dur;
            if (!wasPaused) {{
                const playPromise = audio.play();
                if (playPromise !== undefined) {{
                    playPromise.catch(e => console.log('Audio play error:', e));
                }}
            }}
            renderAll();
        }}
    }});

    function setupSegmentClickListener(canvasId, segOffset) {{
        const canvasElem = document.getElementById(canvasId);
        if (!canvasElem) return;

        canvasElem.addEventListener('click', (e) => {{
            const curTimeMs = (audio.currentTime || 0) * 1000.0;
            const curSegIdx = Math.floor(curTimeMs / bestBarLengthMs);
            const targetSegIdx = curSegIdx + segOffset;
            const seg = segmentsData.find(s => s.segment_index === targetSegIdx);
            if (!seg) return;

            const rect = canvasElem.getBoundingClientRect();
            const clickX = e.clientX - rect.left;
            const clickFraction = Math.max(0, Math.min(1, clickX / rect.width));

            const segDurMs = seg.end_ms - seg.start_ms;
            const targetTimeMs = seg.start_ms + clickFraction * segDurMs;
            const targetTimeS = targetTimeMs / 1000.0;
            const dur = (audio.duration && !isNaN(audio.duration) && audio.duration > 0) ? audio.duration : totalDurationS;

            if (targetTimeS >= 0 && targetTimeS <= dur) {{
                const wasPaused = audio.paused;
                audio.currentTime = targetTimeS;
                if (!wasPaused) {{
                    const playPromise = audio.play();
                    if (playPromise !== undefined) {{
                        playPromise.catch(e => console.log('Audio play error:', e));
                    }}
                }}
                renderAll();
            }}
        }});
    }}

    setupSegmentClickListener('canvasPrev', -1);
    setupSegmentClickListener('canvasCurr', 0);
    setupSegmentClickListener('canvasNext', 1);
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


def analyze_cumulative_transience_high_points(ctbin_manifest, gui_mode=True):
    """
    Performs first-pass cumulative transience analysis using the ctbin manifest loaded from disk.
    Tracks the x-axis value (ms) of the high point in the cumulative history graph across frames,
    determines the high point value that persisted for the longest total duration over the course
    of the audio file, and records change events where the high point value transitioned.

    Returns:
      longest_high_point_ms: float (the x-axis value of the high point active for the longest duration)
      hp_changes: list of dicts [{'time_s': float, 'new_value_ms': float}] recording change events
    """
    if not ctbin_manifest:
        print("Warning: Cumulative transience analysis yielded no high point history data.")
        return float(MIN_SEGMENT_LEN_MS), []

    sample_rate = ctbin_manifest.get('sample_rate', 44100)
    num_frames = ctbin_manifest.get('num_frames', 0)
    frame_dt_s = 0.001

    peaks = ctbin_manifest.get('peaks', [])
    highest_peaks_ms = []
    times_s = [i * frame_dt_s for i in range(num_frames)]

    # Extract high points time-series from peaks or fallback
    if 'highest_peaks_ms' in ctbin_manifest:
        highest_peaks = ctbin_manifest['highest_peaks_ms']
    else:
        # Reconstruct highest peaks time series per frame from peak time_ms if available
        highest_peaks = [-999.0] * num_frames
        for p in peaks:
            t_ms = p.get('time_ms', 0.0)
            f_idx = int(round(t_ms))
            if 0 <= f_idx < num_frames:
                highest_peaks[f_idx] = t_ms

    times = times_s

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
        return float(MIN_SEGMENT_LEN_MS), []

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

    return longest_high_point_ms, hp_changes


def analyze_segment_length(segments_transience_data):
    """
    Analyzes contiguous segments using cumulative transience segment ratings to group
    contiguous patterns.
    Pattern rating = min(segment_ratings in pattern) * number_of_segments.
    A segment is included in a potential pattern if:
      1. Its inclusion strictly raises the overall pattern rating (rating_with > rating_without).
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

        # Exclude segment if it does not strictly raise the overall pattern rating OR if it's better off on its own
        if rating_with <= rating_without or standalone_rating > rating_with:
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

    output_dir = os.path.splitext(audio_path)[0]
    os.makedirs(output_dir, exist_ok=True)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    ct_exec = os.path.join(script_dir, "ct_analyze_headless")
    if os.name == "nt" or sys.platform.startswith("win"):
        ct_exec += ".exe"

    if not os.path.exists(ct_exec):
        print(f"Headless executable not found at '{ct_exec}'. Attempting to compile via make...")
        try:
            subprocess.run(["make", "-C", script_dir], check=True)
        except Exception as e:
            print(f"Error compiling headless executable: {e}")

    stem_name = os.path.splitext(os.path.basename(audio_path))[0]
    ctbin_path = os.path.join(output_dir, f"{stem_name}.ctbin")

    # Execute standalone C headless analyzer executable if .ctbin does not already exist
    if not os.path.exists(ctbin_path) and os.path.exists(ct_exec):
        try:
            print(f"Running C headless analyzer: {ct_exec} {audio_path} 15000")
            subprocess.run([ct_exec, audio_path, "15000"], check=True)
        except Exception as e:
            print(f"Error executing C headless analyzer: {e}")

    ctbin_manifest = export_ctbin_assets(ctbin_path, output_dir)

    # First pass: Cumulative transience high point analysis using C export manifest
    longest_hp_ms, hp_changes = analyze_cumulative_transience_high_points(
        ctbin_manifest=ctbin_manifest,
        gui_mode=gui_mode
    )

    best_bar_length = longest_hp_ms

    # Pass 2: Re-run headless analysis with window_ms = segment length * 2 if bar length differs significantly
    pass2_win_ms = min(15000, int(round(best_bar_length * 2)))
    if os.path.exists(ct_exec) and pass2_win_ms != 15000:
        try:
            print(f"Running Pass 2 C headless analyzer: {ct_exec} {audio_path} {pass2_win_ms}")
            subprocess.run([ct_exec, audio_path, str(pass2_win_ms)], check=True)
            ctbin_manifest = export_ctbin_assets(ctbin_path, output_dir)
        except Exception as e:
            print(f"Error running Pass 2 C headless analyzer: {e}")

    analysis_res = ctbin_manifest or {}

    # Extract all peaks across bands for segment-based transience scoring
    all_peaks_flat = []
    if ctbin_manifest and 'peaks' in ctbin_manifest:
        for p in ctbin_manifest['peaks']:
            if isinstance(p, dict):
                p_copy = dict(p)
                p_copy.pop('snapshot', None)
                all_peaks_flat.append(p_copy)
            else:
                all_peaks_flat.append(p)

    print(f"\nAnalyzing segment length determined from cumulative history high point: {best_bar_length:.2f} ms...")

    # Compute offline segment transience scoring & ratings
    segments_transience_data = compute_offline_segment_transience(
        all_peaks_flat=all_peaks_flat,
        total_duration_ms=total_duration_ms,
        bar_length_ms=best_bar_length
    )

    best_patterns, max_total_pattern_len_ms = analyze_segment_length(segments_transience_data)

    print("\n" + "="*60)
    print(f"ANALYSIS COMPLETE: Bar length determined to be {best_bar_length:.2f} ms")
    print(f"Total pattern duration at bar length: {max_total_pattern_len_ms:.2f} ms")
    print(f"Number of patterns identified: {len(best_patterns)}")
    print("="*60)

    # Save each identified pattern as its own WAV file
    if best_patterns:
        print(f"\nSaving {len(best_patterns)} identified pattern WAV file(s) to '{output_dir}'...")
        for pat_idx, pat in enumerate(best_patterns, 1):
            start_sample = max(0, int(round((pat['start_ms'] / 1000.0) * orig_sr)))
            end_sample = min(len(raw_y), int(round((pat['end_ms'] / 1000.0) * orig_sr)))
            pat_y = raw_y[start_sample:end_sample]
            pat_wav_filename = f"pattern_{pat_idx}.wav"
            pat_wav_path = os.path.join(output_dir, pat_wav_filename)
            try:
                sf.write(pat_wav_path, pat_y, orig_sr)
                print(f"  Saved {pat_wav_filename} ({pat['duration_ms']:.2f} ms)")
            except Exception as e:
                print(f"  Error saving {pat_wav_filename}: {e}")

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

    # Export Interactive HTML Report (Saved to disk for user to click and open whenever needed)
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
                open_browser=False,
                pass2_res=analysis_res,
                ctbin_manifest=ctbin_manifest
            )
        except Exception as e:
            print(f"Could not generate interactive HTML report: {e}")
            traceback.print_exc()

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
    except BaseException as e:
        print("\n" + "="*60)
        print("ERROR OCCURRED")
        print("="*60)
        traceback.print_exc()
        print("="*60)
        if sys.stdin.isatty():
            try:
                input("\nAn error occurred. Press Enter to exit...")
            except BaseException:
                pass
    else:
        if sys.stdin.isatty():
            try:
                input("\nProcess completed successfully. Press Enter to exit...")
            except BaseException:
                pass

import pyray as pr
import math
import bisect
import numpy as np
import os

# Color constants
COLOR_BG = pr.Color(18, 18, 22, 255)       # Deep charcoal background
COLOR_GRID = pr.Color(40, 40, 48, 255)     # Grid lines
COLOR_TEXT_MUTED = pr.Color(140, 140, 150, 255)

# Band colors (Flux/Smooth)
BAND_COLORS = [
    pr.Color(27, 79, 114, 255),    # Sub-Bass
    pr.Color(52, 152, 219, 255),   # Bass/Low-Mid
    pr.Color(46, 204, 113, 255),   # High-Mid
    pr.Color(169, 223, 191, 255)   # Treble
]

# Prominence colors (Distinguishable Red/Orange shades)
PROMINENCE_COLORS = [
    pr.Color(255, 0, 0, 255),      # Red
    pr.Color(255, 69, 0, 255),     # OrangeRed
    pr.Color(255, 99, 71, 255),    # Tomato
    pr.Color(205, 92, 92, 255)     # IndianRed
]

COLOR_PLAYHEAD = pr.Color(230, 126, 34, 255)     # Orange
COLOR_CLEANUP = pr.Color(155, 89, 182, 255)      # Purple
COLOR_PEAK_MARKER = pr.Color(241, 196, 15, 255)  # Yellow
COLOR_HIST_WAVE = pr.Color(241, 196, 15, 255)    # Yellow
COLOR_MIDPOINT = pr.Color(128, 128, 128, 255)    # Gray

BAND_LABELS = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble']
SNAP_LABELS = ['Sub', 'Bass', 'Mid', 'Hi']

_FONT_CACHE = None

def reset_font_cache():
    global _FONT_CACHE
    _FONT_CACHE = None

def get_font():
    global _FONT_CACHE
    if _FONT_CACHE is not None:
        return _FONT_CACHE

    font_candidates = []

    try:
        import matplotlib.font_manager as fm
        for fam in ['DejaVu Sans', 'Arial', 'Liberation Sans', 'sans-serif']:
            fpath = fm.findfont(fm.FontProperties(family=fam))
            if fpath and os.path.exists(fpath) and fpath.lower().endswith('.ttf'):
                if fpath not in font_candidates:
                    font_candidates.append(fpath)
    except Exception:
        pass

    sys_paths = [
        r"C:\Windows\Fonts\segoeui.ttf",
        r"C:\Windows\Fonts\calibri.ttf",
        r"C:\Windows\Fonts\arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]

    for p in sys_paths:
        if os.path.exists(p) and p not in font_candidates and p.lower().endswith('.ttf'):
            font_candidates.append(p)

    for path in font_candidates:
        try:
            f = pr.load_font_ex(path, 48, None, 0)
            if f and hasattr(f, 'texture') and f.texture.id > 0 and hasattr(f, 'glyphCount') and f.glyphCount > 0:
                pr.set_texture_filter(f.texture, pr.TEXTURE_FILTER_BILINEAR)
                _FONT_CACHE = f
                return _FONT_CACHE
            elif f and hasattr(f, 'baseSize') and f.baseSize > 0:
                try:
                    pr.unload_font(f)
                except Exception:
                    pass
        except Exception:
            pass

    _FONT_CACHE = pr.get_font_default()
    return _FONT_CACHE

def hex_to_color(hex_str, alpha=255):
    hex_str = hex_str.lstrip('#')
    r = int(hex_str[0:2], 16)
    g = int(hex_str[2:4], 16)
    b = int(hex_str[4:6], 16)
    return pr.Color(r, g, b, alpha)

def get_score_color(score, min_score, max_score):
    if score == 0: return pr.Color(128, 128, 128, 255)
    if score < 0:
        t = score / min_score if min_score < 0 else 0.0
        t = max(0.0, min(1.0, t))
        r = int(128 + (255 - 128) * t)
        g = int(128 + (0 - 128) * t)
        b = int(128 + (0 - 128) * t)
    else:
        t = score / max_score if max_score > 0 else 0.0
        t = max(0.0, min(1.0, t))
        r = int(128 + (0 - 128) * t)
        g = int(128 + (255 - 128) * t)
        b = int(128 + (0 - 128) * t)
    return pr.Color(r, g, b, 255)

def draw_dashed_line(x1, y1, x2, y2, color, thickness=1, dash_len=5, gap_len=5):
    dx = x2 - x1
    dy = y2 - y1
    dist = math.hypot(dx, dy)
    if dist == 0: return
    ux = dx / dist
    uy = dy / dist
    curr = 0.0
    while curr < dist:
        nxt = min(dist, curr + dash_len)
        pr.draw_line_ex(
            pr.Vector2(x1 + ux * curr, y1 + uy * curr),
            pr.Vector2(x1 + ux * nxt, y1 + uy * nxt),
            thickness, color
        )
        curr += dash_len + gap_len

def draw_text_safe(text, x, y, size, color):
    if text is None: return
    font = get_font()
    if font and hasattr(font, 'baseSize') and font.baseSize > 0:
        pos = pr.Vector2(float(x), float(y))
        pr.draw_text_ex(font, str(text), pos, float(size), 1.0, color)
    else:
        pr.draw_text(str(text), int(x), int(y), int(size), color)

def draw_renderer(W, H, current_time, frame_data):
    """
    Renders the entire 3-panel visualization onto the current drawing target/window.
    W, H: Target width and height
    current_time: Time of the current playhead in seconds
    frame_data: Dict containing metrics, buffers, and historical lists
    """
    pr.clear_background(COLOR_BG)

    log_enabled = frame_data.get('log_enabled', 0)
    H_console = 220 if log_enabled else 0
    H_eff = H - H_console

    # Panel Heights
    H_top = int(H_eff * 1.0 / 2.4)
    H_mid = int(H_eff * 0.4 / 2.4)
    H_bot = int(H_eff * 1.0 / 2.4)

    Y_top = 0
    Y_mid = H_top
    Y_bot = H_top + H_mid

    # Margins and dimensions
    margin_left = int(W * 0.08)
    margin_right = int(W * 0.15)
    margin_top = int(H_eff * 0.04)
    margin_bottom = int(H_eff * 0.04)

    # Extract dynamic/shared boundaries
    min_score = frame_data.get('min_score_seen', -5.0)
    max_score = frame_data.get('max_score_seen', 5.0)

    # -------------------------------------------------------------
    # 1. TOP PANEL: 4-Band Transient Envelopes
    # -------------------------------------------------------------
    graph_w = W - margin_left - margin_right
    graph_h_top = H_top - margin_top - int(H_top * 0.1)

    # Axes limits: current_time - 20s to current_time + 5s
    x_min_t = current_time - 20.0
    x_max_t = current_time + 5.0
    x_span_t = 25.0

    # Waveforms/Envelopes data
    times_arr = frame_data.get('times', [])
    onset_envs = frame_data.get('onset_envs', [])
    smooth_envs = frame_data.get('smooth_envs', [])
    prom_envs = frame_data.get('prominences', [])

    # Calculate dynamic max peak visible in the current 25s window
    local_max = 0.0
    if len(times_arr) > 1 and onset_envs:
        idx_start = bisect.bisect_left(times_arr, x_min_t)
        idx_end = bisect.bisect_right(times_arr, x_max_t)
        if idx_end > idx_start:
            for b in range(min(4, len(onset_envs))):
                b_slice = onset_envs[b][idx_start:idx_end]
                if len(b_slice) > 0:
                    m_val = float(np.max(b_slice)) if hasattr(b_slice, 'tolist') else max(b_slice)
                    if m_val > local_max:
                        local_max = m_val

    if local_max > 0:
        max_peak = local_max * 1.1
    else:
        max_peak = frame_data.get('max_peak_value', 1.0)
        if max_peak <= 0: max_peak = 1.0

    # Draw top border/grid
    pr.draw_rectangle_lines(margin_left, margin_top, graph_w, graph_h_top, COLOR_GRID)

    # Draw vertical grids (every 5 seconds)
    start_tick = math.floor(x_min_t / 5.0) * 5.0
    while start_tick <= x_max_t:
        if start_tick >= x_min_t:
            gx = margin_left + int(graph_w * (start_tick - x_min_t) / x_span_t)
            pr.draw_line(gx, margin_top, gx, margin_top + graph_h_top, COLOR_GRID)
            # Time labels (mm:ss)
            total_sec = abs(int(start_tick))
            m = total_sec // 60
            s = total_sec % 60
            prefix = "-" if start_tick < 0 else ""
            lbl = f"{prefix}{m}:{s:02d}"
            draw_text_safe(lbl, gx - 15, margin_top + graph_h_top + 5, 12, COLOR_TEXT_MUTED)
        start_tick += 5.0

    # Draw Y-axis ticks, horizontal grid lines, and tick labels for Top Panel
    for ratio in [0.0, 0.25, 0.5, 0.75, 1.0]:
        gy = margin_top + graph_h_top - int(graph_h_top * ratio)
        if 0.0 < ratio < 1.0:
            pr.draw_line(margin_left, gy, margin_left + graph_w, gy, COLOR_GRID)
        pr.draw_line(margin_left - 5, gy, margin_left, gy, COLOR_TEXT_MUTED)
        val = max_peak * ratio
        lbl = f"{val:.2f}"
        draw_text_safe(lbl, margin_left - 42, gy - 6, 11, COLOR_TEXT_MUTED)

    draw_text_safe("Onset Strength", 10, margin_top - 18, 11, COLOR_TEXT_MUTED)

    # Waveforms/Envelopes rendering
    if len(times_arr) > 1 and onset_envs:
        for b in range(4):
            env_pts = []
            sm_pts = []
            pr_pts = []

            # Find visible range in times_arr
            idx_start = bisect.bisect_left(times_arr, x_min_t)
            idx_end = bisect.bisect_right(times_arr, x_max_t)
            num_visible = idx_end - idx_start

            if num_visible > 0:
                if num_visible <= 1000:
                    for idx in range(idx_start, idx_end):
                        t = times_arr[idx]
                        sx = margin_left + int(graph_w * (t - x_min_t) / x_span_t)

                        f_val = onset_envs[b][idx] if b < len(onset_envs) else 0.0
                        sy_f = margin_top + graph_h_top - int(graph_h_top * f_val / max_peak)
                        env_pts.append(pr.Vector2(sx, sy_f))

                        if smooth_envs and b < len(smooth_envs):
                            s_val = smooth_envs[b][idx]
                            sy_s = margin_top + graph_h_top - int(graph_h_top * s_val / max_peak)
                            sm_pts.append(pr.Vector2(sx, sy_s))

                        if prom_envs and b < len(prom_envs):
                            p_val = prom_envs[b][idx]
                            sy_p = margin_top + graph_h_top - int(graph_h_top * p_val / max_peak)
                            pr_pts.append(pr.Vector2(sx, sy_p))
                else:
                    target_bins = 300
                    bin_size = num_visible / float(target_bins)
                    for k in range(target_bins):
                        bin_start = idx_start + int(k * bin_size)
                        bin_end = idx_start + int((k + 1) * bin_size)
                        bin_end = min(bin_end, idx_end)
                        if bin_end <= bin_start:
                            bin_end = bin_start + 1

                        t_bin = times_arr[bin_start]
                        sx = margin_left + int(graph_w * (t_bin - x_min_t) / x_span_t)

                        # Flux
                        if b < len(onset_envs):
                            slice_f = onset_envs[b][bin_start:bin_end]
                            if len(slice_f) > 0:
                                if hasattr(slice_f, 'tolist'):
                                    max_f = float(np.max(slice_f))
                                else:
                                    max_f = max(slice_f)
                                sy_f = margin_top + graph_h_top - int(graph_h_top * max_f / max_peak)
                                env_pts.append(pr.Vector2(sx, sy_f))

                        # Smooth
                        if smooth_envs and b < len(smooth_envs):
                            slice_s = smooth_envs[b][bin_start:bin_end]
                            if len(slice_s) > 0:
                                if hasattr(slice_s, 'tolist'):
                                    max_s = float(np.max(slice_s))
                                else:
                                    max_s = max(slice_s)
                                sy_s = margin_top + graph_h_top - int(graph_h_top * max_s / max_peak)
                                sm_pts.append(pr.Vector2(sx, sy_s))

                        # Prominences
                        if prom_envs and b < len(prom_envs):
                            slice_p = prom_envs[b][bin_start:bin_end]
                            if len(slice_p) > 0:
                                if hasattr(slice_p, 'tolist'):
                                    max_p = float(np.max(slice_p))
                                else:
                                    max_p = max(slice_p)
                                sy_p = margin_top + graph_h_top - int(graph_h_top * max_p / max_peak)
                                pr_pts.append(pr.Vector2(sx, sy_p))

            # Draw Flux
            for i in range(len(env_pts) - 1):
                pr.draw_line_ex(env_pts[i], env_pts[i+1], 1.0, pr.Color(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b, 76))

            # Draw Smoothings
            for i in range(len(sm_pts) - 1):
                pr.draw_line_ex(sm_pts[i], sm_pts[i+1], 1.5, pr.Color(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b, 128))

            # Draw Prominences
            for i in range(len(pr_pts) - 1):
                pr.draw_line_ex(pr_pts[i], pr_pts[i+1], 1.5, PROMINENCE_COLORS[b])

    # Average Smoothing horizontal lines
    smoothing_avgs = frame_data.get('rolling_smoothing_avgs', [0.0]*4)
    for b in range(4):
        if b < len(smoothing_avgs):
            s_avg = smoothing_avgs[b]
            sy_avg = margin_top + graph_h_top - int(graph_h_top * s_avg / max_peak)
            if margin_top <= sy_avg <= margin_top + graph_h_top:
                draw_dashed_line(margin_left, sy_avg, margin_left + graph_w, sy_avg, PROMINENCE_COLORS[b], 1.2, 8, 8)
                lbl_text = f"S:{s_avg:.2f}"
                draw_text_safe(lbl_text, margin_left + graph_w + 5, sy_avg - 6, 11, PROMINENCE_COLORS[b])

    # Global Average Smoothing guide
    g_smooth_avg = frame_data.get('rolling_global_smoothing_avg', 0.0)
    if g_smooth_avg > 0:
        sy_g = margin_top + graph_h_top - int(graph_h_top * g_smooth_avg / max_peak)
        if margin_top <= sy_g <= margin_top + graph_h_top:
            pr.draw_line_ex(pr.Vector2(margin_left, sy_g), pr.Vector2(margin_left + graph_w, sy_g), 1.5, pr.BLACK)
            draw_text_safe(f"G:{g_smooth_avg:.2f}", margin_left + graph_w + 50, sy_g - 6, 12, pr.Color(20, 20, 20, 255))

    # Playhead Line (current_time)
    px = margin_left + int(graph_w * (current_time - x_min_t) / x_span_t)
    if margin_left <= px <= margin_left + graph_w:
        draw_dashed_line(px, margin_top, px, margin_top + graph_h_top, COLOR_PLAYHEAD, 2, 6, 4)

    # Cleanup Sweep Line (current_time - 15)
    cx = margin_left + int(graph_w * (current_time - 15.0 - x_min_t) / x_span_t)
    if margin_left <= cx <= margin_left + graph_w:
        draw_dashed_line(cx, margin_top, cx, margin_top + graph_h_top, COLOR_CLEANUP, 2, 2, 2)

    # Live Peak Scatter Markers ('x')
    peaks = frame_data.get('peaks', [])
    for p in peaks:
        pt = p.get('time', 0.0)
        if x_min_t <= pt <= x_max_t:
            pk_x = margin_left + int(graph_w * (pt - x_min_t) / x_span_t)
            pk_val = p.get('peak_val', 0.0)
            pk_y = margin_top + graph_h_top - int(graph_h_top * pk_val / max_peak)
            if margin_top <= pk_y <= margin_top + graph_h_top:
                pr.draw_line(pk_x - 4, pk_y - 4, pk_x + 4, pk_y + 4, COLOR_PEAK_MARKER)
                pr.draw_line(pk_x - 4, pk_y + 4, pk_x + 4, pk_y - 4, COLOR_PEAK_MARKER)

                age = current_time - pt
                if 0 <= age <= 2.0:
                    progress = age / 2.0
                    float_y = pk_y - int(progress * 50.0)
                    alpha = int(255 * (1.0 - progress))
                    score = p.get('total_score', 0.0)
                    clr = get_score_color(score, min_score, max_score)
                    clr = pr.Color(clr.r, clr.g, clr.b, alpha)
                    draw_text_safe(f"{score:+.2f}", pk_x - 10, float_y, 14, clr)

    # Overlays in top-left
    valid_peaks = [p for p in frame_data.get('peaks', []) if p.get('time', 0.0) <= current_time]
    if valid_peaks:
        latest_p_idx = max(p.get('p_idx', 0) for p in valid_peaks)
        window_peaks = [
            p for p in valid_peaks
            if -29.0 <= float(p.get('p_idx', 0) - latest_p_idx) <= 1.0
        ]
        if window_peaks:
            score_avg = sum(p.get('total_score', 0.0) for p in window_peaks) / len(window_peaks)
        else:
            score_avg = 0.0
    else:
        score_avg = 0.0

    all_time_scores = frame_data.get('all_time_scores', [])
    if all_time_scores:
        rating_val = sum(all_time_scores) / len(all_time_scores)
    else:
        rating_val = frame_data.get('rating', 0.0)
    score_clr = get_score_color(score_avg, min_score, max_score)

    draw_text_safe(f"Score: {score_avg:+.2f}", margin_left + 15, margin_top + 15, 24, score_clr)
    draw_text_safe(f"Rating: {rating_val:.2f}", margin_left + 15, margin_top + 45, 14, COLOR_PEAK_MARKER)

    # Document labels in top right margin
    for b in range(4):
        lx = margin_left + graph_w + 5
        ly = margin_top + b * 20
        pr.draw_rectangle(lx, ly, 10, 10, BAND_COLORS[b])
        draw_text_safe(BAND_LABELS[b], lx + 15, ly, 11, COLOR_TEXT_MUTED)

    # Title
    draw_text_safe(frame_data.get('title', "Cumulative Transience Analyzer"), margin_left, margin_top - 20, 14, pr.WHITE)

    # -------------------------------------------------------------
    # 2. MIDDLE PANEL: 29ms Rolling Window Snapshot (Lanes)
    # -------------------------------------------------------------
    graph_h_mid = H_mid - int(H_mid * 0.20)
    lane_h = graph_h_mid / 4.0

    x_min_s = -29.0
    x_max_s = 1.0
    x_span_s = 30.0

    for b in range(4):
        ly = Y_mid + int(b * lane_h)
        if b % 2 == 0:
            pr.draw_rectangle(margin_left, ly, graph_w, int(lane_h), pr.Color(25, 25, 30, 255))
        else:
            pr.draw_rectangle(margin_left, ly, graph_w, int(lane_h), pr.Color(20, 20, 24, 255))

        pr.draw_line(margin_left, ly, margin_left + graph_w, ly, COLOR_GRID)
        draw_text_safe(SNAP_LABELS[b], margin_left - 45, ly + int(lane_h * 0.2), 12, pr.WHITE)

    pr.draw_line(margin_left, Y_mid + graph_h_mid, margin_left + graph_w, Y_mid + graph_h_mid, COLOR_GRID)

    # X-axis ticks, vertical grid lines, and millisecond legend labels (-29ms to 0ms)
    for ms_val in [-29, -25, -20, -15, -10, -5, 0]:
        bx = margin_left + int(graph_w * (ms_val - x_min_s) / x_span_s)
        pr.draw_line(bx, Y_mid, bx, Y_mid + graph_h_mid, COLOR_GRID)
        draw_text_safe(f"{ms_val}ms" if ms_val != 0 else "0ms", bx - 12, Y_mid + graph_h_mid + 4, 11, COLOR_TEXT_MUTED)

    valid_peaks = [p for p in peaks if p.get('time', 0.0) <= current_time]
    if valid_peaks:
        latest_p_idx = max(p.get('p_idx', 0) for p in valid_peaks)
        for p in valid_peaks:
            p_idx = p.get('p_idx', 0)
            rel_ms = float(p_idx - latest_p_idx)
            if -29.0 <= rel_ms <= 1.0:
                band = p.get('band_idx', 0)
                score = p.get('total_score', 0.0)

                bx = margin_left + int(graph_w * (rel_ms - x_min_s) / x_span_s)
                by = Y_mid + int(band * lane_h) + 2
                bw = 2

                pr.draw_rectangle(bx - bw//2, by, bw, int(lane_h) - 4, BAND_COLORS[band])

                score_c = get_score_color(score, min_score, max_score)
                if rel_ms < -25.0:
                    text_x = bx + bw // 2 + 5
                else:
                    text_x = bx - bw // 2 - 40
                draw_text_safe(f"{score:+.2f}", text_x, by + int(lane_h*0.1), 12, score_c)

    draw_text_safe("29ms Rolling Window Snapshot", margin_left, Y_mid - 15, 12, pr.WHITE)

    # -------------------------------------------------------------
    # 3. BOTTOM PANEL: Accumulated 5s Historical Buffer
    # -------------------------------------------------------------
    graph_h_bot = H_bot - margin_bottom - int(H_bot * 0.1)

    x_min_b = -5000.0
    x_max_b = 0.0
    x_span_b = 5000.0

    pr.draw_rectangle_lines(margin_left, Y_bot, graph_w, graph_h_bot, COLOR_GRID)

    for s_idx in range(-5000, 1, 1000):
        bx = margin_left + int(graph_w * (s_idx - x_min_b) / x_span_b)
        pr.draw_line(bx, Y_bot, bx, Y_bot + graph_h_bot, COLOR_GRID)
        draw_text_safe(f"{s_idx}ms" if s_idx != 0 else "0ms", bx - 20, Y_bot + graph_h_bot + 5, 11, COLOR_TEXT_MUTED)

    accum_buffer = frame_data.get('accumulated_buffer', None)
    if accum_buffer is not None and len(accum_buffer) > 0:
        cutoff_idx = int(5001 - (99.0 / 5000.0 * 5000.0))
        visible_sub = accum_buffer[:cutoff_idx] if len(accum_buffer) >= cutoff_idx else accum_buffer
        cur_max = max(visible_sub) if len(visible_sub) > 0 else 1.0
        cur_min = min(visible_sub) if len(visible_sub) > 0 else 0.0
        if cur_max <= 0: cur_max = 1.0
    else:
        cur_max = 1.0
        cur_min = 0.0

    # Draw Y-axis ticks, horizontal grid lines, and tick labels for Bottom Panel
    for ratio in [0.0, 0.25, 0.5, 0.75, 1.0]:
        gy = Y_bot + graph_h_bot - int(graph_h_bot * ratio)
        if 0.0 < ratio < 1.0:
            pr.draw_line(margin_left, gy, margin_left + graph_w, gy, COLOR_GRID)
        pr.draw_line(margin_left - 5, gy, margin_left, gy, COLOR_TEXT_MUTED)
        val = cur_max * ratio
        lbl = f"{val:.2f}"
        draw_text_safe(lbl, margin_left - 42, gy - 6, 11, COLOR_TEXT_MUTED)

    draw_text_safe("Accumulated Energy", 5, Y_bot - 18, 10, COLOR_TEXT_MUTED)

    if accum_buffer is not None and len(accum_buffer) > 0:
        wave_pts = []
        step = max(1, len(accum_buffer) // 1000)
        for i in range(0, len(accum_buffer), step):
            ms_val = -5000.0 + (i / (len(accum_buffer) - 1)) * 5000.0
            val = accum_buffer[i]

            wx = margin_left + int(graph_w * (ms_val - x_min_b) / x_span_b)
            wy = Y_bot + graph_h_bot - int(graph_h_bot * val / cur_max)
            wave_pts.append(pr.Vector2(wx, wy))

        for i in range(len(wave_pts) - 1):
            pr.draw_line_ex(wave_pts[i], wave_pts[i+1], 2.0, COLOR_HIST_WAVE)

        midpoint = (cur_min + cur_max) / 2.0
        wy_mid = Y_bot + graph_h_bot - int(graph_h_bot * midpoint / cur_max)
        if Y_bot <= wy_mid <= Y_bot + graph_h_bot:
            draw_dashed_line(margin_left, wy_mid, margin_left + graph_w, wy_mid, COLOR_MIDPOINT, 1.2, 5, 5)

    if valid_peaks:
        latest_active_peak = max(valid_peaks, key=lambda p: p.get('time', 0.0))
        qualifiers = latest_active_peak.get('qualifiers', [])
        tolerance = frame_data.get('tolerance', 29.0)

        for q in qualifiers:
            q_ms = q.get('ms', 0.0)
            q_val = q.get('val', 0.0)
            q_orig_ms = q.get('orig_ms', q_ms)

            qx = margin_left + int(graph_w * (q_ms - x_min_b) / x_span_b)
            q_orig_x = margin_left + int(graph_w * (q_orig_ms - x_min_b) / x_span_b)

            tol_w = int(graph_w * tolerance / x_span_b)

            if margin_left <= qx <= margin_left + graph_w:
                qc = get_score_color(q_val, -1.0, 1.0)
                pr.draw_line_ex(pr.Vector2(qx, Y_bot), pr.Vector2(qx, Y_bot + graph_h_bot), 2.5, qc)

                span_x = q_orig_x - tol_w
                span_w = 2 * tol_w
                if span_x < margin_left:
                    span_w -= (margin_left - span_x)
                    span_x = margin_left
                if span_x + span_w > margin_left + graph_w:
                    span_w = (margin_left + graph_w) - span_x

                if span_w > 0:
                    pr.draw_rectangle(span_x, Y_bot, span_w, graph_h_bot, pr.Color(qc.r, qc.g, qc.b, 38))

                lbl_y = Y_bot + int(graph_h_bot * (1.0 - (q_val + 1.0)/2.0))
                draw_text_safe(f"{q_val:+.2f}", qx + 4, lbl_y, 10, qc)

    highest_peak_ms = frame_data.get('highest_peak_ms', -999.0)
    if highest_peak_ms > -900.0:
        hx = margin_left + int(graph_w * (highest_peak_ms - x_min_b) / x_span_b)
        if margin_left <= hx <= margin_left + graph_w:
            draw_dashed_line(hx, Y_bot, hx, Y_bot + graph_h_bot, COLOR_PEAK_MARKER, 2, 4, 4)

    std_dev = frame_data.get('std_dev', 0.0)
    contrast = frame_data.get('contrast', 0.0)
    stability = frame_data.get('stability', 0.0)

    draw_text_safe(f"Std Dev: {std_dev:.3f}", margin_left + 15, Y_bot + 15, 12, COLOR_PEAK_MARKER)
    draw_text_safe(f"Contrast: {contrast:.3f}", margin_left + 15, Y_bot + 35, 12, COLOR_PEAK_MARKER)
    draw_text_safe(f"Stability: {stability:.0f}", margin_left + 15, Y_bot + 55, 12, COLOR_PEAK_MARKER)

    group_val = frame_data.get('group', '')
    if group_val:
        group_lbl = f"  -  Group: @{group_val}"
    else:
        group_lbl = "  -  Group: [No Group]"
    draw_text_safe(f"Accumulated 5s Historical Buffer{group_lbl}", margin_left + 35, Y_bot - 18, 12, COLOR_PEAK_MARKER)

    # -------------------------------------------------------------
    # 4. CONSOLE PANEL: TCP Packet Log (when @log mode is enabled)
    # -------------------------------------------------------------
    if log_enabled and H_console > 0:
        Y_console = H_eff
        console_h = H_console - margin_bottom
        pr.draw_rectangle(margin_left, Y_console, graph_w, console_h, pr.Color(12, 12, 15, 255))
        pr.draw_rectangle_lines(margin_left, Y_console, graph_w, console_h, COLOR_GRID)
        draw_text_safe("TCP Packet Log", margin_left + 10, Y_console + 5, 12, COLOR_PEAK_MARKER)

        packet_logs = frame_data.get('packet_logs', [])
        char_width = 7.0
        max_chars_per_line = max(20, int((graph_w - 20) / char_width))

        wrapped_lines = []
        for raw_pkt in packet_logs:
            # Wrap long packet lines cleanly across available width
            idx = 0
            while idx < len(raw_pkt):
                wrapped_lines.append(raw_pkt[idx:idx + max_chars_per_line])
                idx += max_chars_per_line

        # Show as many lines as can fit vertically in the console area
        max_visible_text_lines = max(1, int((console_h - 30) / 15))
        visible_wrapped = wrapped_lines[-max_visible_text_lines:]

        log_y = Y_console + 24
        for wline in visible_wrapped:
            draw_text_safe(wline, margin_left + 10, log_y, 11, COLOR_TEXT_MUTED)
            log_y += 15

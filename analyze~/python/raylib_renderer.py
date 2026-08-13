import pyray as pr
import math

def make_color(r, g, b, a=255):
    # Dynamic self-healing Color constructor that adapts to any installed Python Raylib binding variation
    if hasattr(pr, 'Color'):
        try:
            return pr.Color(int(r), int(g), int(b), int(a))
        except Exception:
            pass
    if hasattr(pr, 'color'):
        try:
            return pr.color(int(r), int(g), int(b), int(a))
        except Exception:
            pass
    # Pack into 32-bit int and try get_color / GetColor
    val = ((int(r) & 0xff) << 24) | ((int(g) & 0xff) << 16) | ((int(b) & 0xff) << 8) | (int(a) & 0xff)
    if hasattr(pr, 'get_color'):
        try:
            return pr.get_color(val)
        except Exception:
            pass
    if hasattr(pr, 'GetColor'):
        try:
            return pr.GetColor(val)
        except Exception:
            pass
    raise AttributeError("Could not find a valid Color constructor in the installed 'pyray' module. Please ensure the standard 'raylib' package is installed: pip install raylib")

# Color constants using the dynamic constructor
COLOR_BG = make_color(18, 18, 22, 255)       # Deep charcoal background
COLOR_GRID = make_color(40, 40, 48, 255)     # Grid lines
COLOR_TEXT_MUTED = make_color(140, 140, 150, 255)

# Band colors (Flux/Smooth)
BAND_COLORS = [
    make_color(27, 79, 114, 255),    # Sub-Bass
    make_color(52, 152, 219, 255),   # Bass/Low-Mid
    make_color(46, 204, 113, 255),   # High-Mid
    make_color(169, 223, 191, 255)   # Treble
]

# Prominence colors (Distinguishable Red/Orange shades)
PROMINENCE_COLORS = [
    make_color(255, 0, 0, 255),      # Red
    make_color(255, 69, 0, 255),     # OrangeRed
    make_color(255, 99, 71, 255),    # Tomato
    make_color(205, 92, 92, 255)     # IndianRed
]

COLOR_PLAYHEAD = make_color(230, 126, 34, 255)     # Orange
COLOR_CLEANUP = make_color(155, 89, 182, 255)      # Purple
COLOR_PEAK_MARKER = make_color(241, 196, 15, 255)  # Yellow
COLOR_HIST_WAVE = make_color(241, 196, 15, 255)    # Yellow
COLOR_MIDPOINT = make_color(128, 128, 128, 255)    # Gray

BAND_LABELS = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble']
SNAP_LABELS = ['Sub', 'Bass', 'Mid', 'Hi']

def hex_to_color(hex_str, alpha=255):
    hex_str = hex_str.lstrip('#')
    r = int(hex_str[0:2], 16)
    g = int(hex_str[2:4], 16)
    b = int(hex_str[4:6], 16)
    return make_color(r, g, b, alpha)

def get_score_color(score, min_score, max_score):
    if score == 0: return make_color(128, 128, 128, 255)
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
    return make_color(r, g, b, 255)

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
    pr.draw_text(str(text), int(x), int(y), int(size), color)

def draw_renderer(W, H, current_time, frame_data):
    """
    Renders the entire 3-panel visualization onto the current drawing target/window.
    W, H: Target width and height
    current_time: Time of the current playhead in seconds
    frame_data: Dict containing metrics, buffers, and historical lists
    """
    pr.clear_background(COLOR_BG)

    # Panel Heights
    H_top = int(H * 1.0 / 2.4)
    H_mid = int(H * 0.4 / 2.4)
    H_bot = int(H * 1.0 / 2.4)

    Y_top = 0
    Y_mid = H_top
    Y_bot = H_top + H_mid

    # Margins and dimensions
    margin_left = int(W * 0.06)
    margin_right = int(W * 0.15)
    margin_top = int(H * 0.04)
    margin_bottom = int(H * 0.04)

    # Extract dynamic/shared boundaries
    min_score = frame_data.get('min_score_seen', -5.0)
    max_score = frame_data.get('max_score_seen', 5.0)
    max_peak = frame_data.get('max_peak_value', 1.0)
    if max_peak <= 0: max_peak = 1.0

    # -------------------------------------------------------------
    # 1. TOP PANEL: 4-Band Transient Envelopes
    # -------------------------------------------------------------
    graph_w = W - margin_left - margin_right
    graph_h_top = H_top - margin_top - int(H_top * 0.1)

    # Axes limits: current_time - 20s to current_time + 5s
    x_min_t = current_time - 20.0
    x_max_t = current_time + 5.0
    x_span_t = 25.0

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

    # Waveforms/Envelopes rendering
    times_arr = frame_data.get('times', [])
    onset_envs = frame_data.get('onset_envs', [])
    smooth_envs = frame_data.get('smooth_envs', [])
    prom_envs = frame_data.get('prominences', [])

    if len(times_arr) > 1 and onset_envs:
        for b in range(4):
            env_pts = []
            sm_pts = []
            pr_pts = []

            step = max(1, len(times_arr) // 1000)
            for idx in range(0, len(times_arr), step):
                t = times_arr[idx]
                if t < x_min_t or t > x_max_t: continue

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

            # Draw Flux
            for i in range(len(env_pts) - 1):
                pr.draw_line_ex(env_pts[i], env_pts[i+1], 1.0, make_color(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b, 76))

            # Draw Smoothings
            for i in range(len(sm_pts) - 1):
                pr.draw_line_ex(sm_pts[i], sm_pts[i+1], 1.5, make_color(BAND_COLORS[b].r, BAND_COLORS[b].g, BAND_COLORS[b].b, 128))

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
            draw_text_safe(f"G:{g_smooth_avg:.2f}", margin_left + graph_w + 50, sy_g - 6, 12, make_color(20, 20, 20, 255))

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
                    clr = make_color(clr.r, clr.g, clr.b, alpha)
                    draw_text_safe(f"{score:+.2f}", pk_x - 10, float_y, 14, clr)

    # Overlays in top-left
    score_avg = frame_data.get('rating', 0.0)
    rating_val = frame_data.get('overall_rating', 0.0)
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
    # 2. MIDDLE PANEL: 39ms Rolling Window Snapshot (Lanes)
    # -------------------------------------------------------------
    graph_h_mid = H_mid - int(H_mid * 0.15)
    lane_h = graph_h_mid / 4.0

    x_min_s = -45.0
    x_max_s = 1.0
    x_span_s = 46.0

    for b in range(4):
        ly = Y_mid + int(b * lane_h)
        if b % 2 == 0:
            pr.draw_rectangle(margin_left, ly, graph_w, int(lane_h), make_color(25, 25, 30, 255))
        else:
            pr.draw_rectangle(margin_left, ly, graph_w, int(lane_h), make_color(20, 20, 24, 255))

        pr.draw_line(margin_left, ly, margin_left + graph_w, ly, COLOR_GRID)
        draw_text_safe(SNAP_LABELS[b], margin_left - 45, ly + int(lane_h * 0.2), 12, pr.WHITE)

    pr.draw_line(margin_left, Y_mid + graph_h_mid, margin_left + graph_w, Y_mid + graph_h_mid, COLOR_GRID)

    for p in peaks:
        pt = p.get('time', 0.0)
        if abs(current_time - pt) < 0.1:
            rel_idx = p.get('p_idx', 0)
            band = p.get('band_idx', 0)
            score = p.get('total_score', 0.0)

            bx = margin_left + int(graph_w * (0.0 - x_min_s) / x_span_s)
            by = Y_mid + int(band * lane_h) + 2
            bw = int(graph_w * 2.0 / x_span_s)
            if bw < 5: bw = 5

            pr.draw_rectangle(bx - bw//2, by, bw, int(lane_h) - 4, BAND_COLORS[band])

            score_c = get_score_color(score, min_score, max_score)
            draw_text_safe(f"{score:+.2f}", bx - 45, by + int(lane_h*0.1), 12, score_c)

    draw_text_safe("39ms Rolling Window Snapshot", margin_left, Y_mid - 15, 12, pr.WHITE)

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

    accum_buffer = frame_data.get('accumulated_buffer', [])
    if accum_buffer:
        cutoff_idx = int(5001 - (99.0 / 5000.0 * 5000.0))
        visible_sub = accum_buffer[:cutoff_idx] if len(accum_buffer) >= cutoff_idx else accum_buffer
        cur_max = max(visible_sub) if visible_sub else 1.0
        cur_min = min(visible_sub) if visible_sub else 0.0
        if cur_max <= 0: cur_max = 1.0

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

    active_peaks_in_chunk = [p for p in peaks if abs(current_time - p.get('time', 0.0)) < 0.1]
    if active_peaks_in_chunk:
        latest_active_peak = active_peaks_in_chunk[-1]
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
                    pr.draw_rectangle(span_x, Y_bot, span_w, graph_h_bot, make_color(qc.r, qc.g, qc.b, 38))

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

    draw_text_safe("Accumulated 5s Historical Buffer", margin_left, Y_bot - 15, 12, pr.WHITE)

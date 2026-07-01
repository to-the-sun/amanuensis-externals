import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont
import os

class OCVRenderer:
    def __init__(self, bg_image, layout_metadata):
        """
        bg_image: numpy array (RGB) of the Matplotlib background.
        layout_metadata: dict containing subplot bboxes and data-to-pixel mapping info.
        """
        self.bg_image = bg_image
        self.height, self.width = bg_image.shape[:2]
        self.meta = layout_metadata

        # Colors (RGB)
        self.band_colors = [
            (27, 79, 114),   # Sub-Bass: #1b4f72
            (52, 152, 219),  # Bass/Low-Mid: #3498db
            (46, 204, 113),  # High-Mid: #2ecc71
            (169, 223, 191)  # Treble: #a9dfbf
        ]
        self.playhead_color = (230, 126, 34) # #e67e22
        self.cleanup_color = (155, 89, 182)  # #9b59b6
        self.buffer_color = (241, 196, 15)   # #f1c40f
        self.mean_color = (128, 128, 128)    # #808080
        self.highlight_color = (241, 196, 15) # #f1c40f

        # Font lookup
        self.fonts = {}
        self._init_fonts()

    def _init_fonts(self):
        # Improved font lookup
        font_names = [
            "DejaVuSans", "DejaVu Sans", "Arial", "Liberation Sans", "FreeSans"
        ]

        potential_dirs = [
            "/usr/share/fonts/truetype/dejavu",
            "/usr/share/fonts/truetype/liberation",
            "/usr/share/fonts/truetype/freefont",
            "C:\\Windows\\Fonts",
            "/System/Library/Fonts"
        ]

        font_path = None
        for d in potential_dirs:
            if not os.path.exists(d): continue
            for f in os.listdir(d):
                if any(name.lower() in f.lower() for name in font_names) and f.lower().endswith(".ttf"):
                    font_path = os.path.join(d, f)
                    break
            if font_path: break

        if font_path:
            self.fonts['metrics'] = ImageFont.truetype(font_path, 10)
            self.fonts['debug'] = ImageFont.truetype(font_path, 12)
            self.fonts['score_big'] = ImageFont.truetype(font_path, 20)
            self.fonts['rating'] = ImageFont.truetype(font_path, 12)
            self.fonts['snapshot_score'] = ImageFont.truetype(font_path, 13)
            self.fonts['popup_score'] = ImageFont.truetype(font_path, 10)
        else:
            # Fallback to default
            default = ImageFont.load_default()
            for k in ['metrics', 'debug', 'score_big', 'rating', 'snapshot_score', 'popup_score']:
                self.fonts[k] = default

    def get_score_color(self, score, min_score, max_score):
        if score == 0: return (128, 128, 128)
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
        return (r, g, b)

    def render_frame(self, frame_data, static_data):
        # We start with the Matplotlib background
        frame = self.bg_image.copy()

        # 1. OpenCV Drawing (Faster for shapes/lines)
        self._render_transient_shapes(frame, frame_data, static_data)
        self._render_snapshot_shapes(frame, frame_data, static_data)
        self._render_buffer_shapes(frame, frame_data, static_data)

        # 2. Pillow Drawing (Better for text)
        # We do this once at the end to minimize conversions
        pil_img = Image.fromarray(frame)
        draw = ImageDraw.Draw(pil_img)
        self._render_text_overlays(draw, frame_data, static_data)

        return np.array(pil_img)

    def _get_ax_coords(self, ax_key, data_x, data_y, dynamic_xlim=None, dynamic_ylim=None):
        meta = self.meta[ax_key]
        x0, y0_top, x1, y1_top = meta['bbox_px']
        xlim = dynamic_xlim if dynamic_xlim else meta['xlim']
        ylim = dynamic_ylim if dynamic_ylim else meta['ylim']

        px = x0 + (data_x - xlim[0]) * (x1 - x0) / (xlim[1] - xlim[0])
        py = y1_top - (data_y - ylim[0]) * (y1_top - y0_top) / (ylim[1] - ylim[0])
        return int(px), int(py)

    def _render_transient_shapes(self, frame, frame_data, static_data):
        meta = self.meta['ax_transient']
        curr_time = static_data['times'][frame_data['frame']]
        xlim = (curr_time - 20, curr_time + 5)

        # Envelopes
        for i in range(4):
            env = static_data['onset_envs'][i]
            idx_start = np.searchsorted(static_data['times'], xlim[0])
            idx_end = np.searchsorted(static_data['times'], xlim[1])

            pts = []
            for idx in range(idx_start, idx_end):
                px, py = self._get_ax_coords('ax_transient', static_data['times'][idx], env[idx], dynamic_xlim=xlim)
                pts.append([px, py])

            if len(pts) > 1:
                cv2.polylines(frame, [np.array(pts)], False, self.band_colors[i], 2, cv2.LINE_AA)

        # Playhead
        px, _ = self._get_ax_coords('ax_transient', curr_time, 0, dynamic_xlim=xlim)
        cv2.line(frame, (px, int(meta['bbox_px'][1])), (px, int(meta['bbox_px'][3])), self.playhead_color, 2, cv2.LINE_4)

        # Cleanup Sweep
        cx, _ = self._get_ax_coords('ax_transient', curr_time - 15, 0, dynamic_xlim=xlim)
        if meta['bbox_px'][0] <= cx <= meta['bbox_px'][2]:
            cv2.line(frame, (cx, int(meta['bbox_px'][1])), (cx, int(meta['bbox_px'][3])), self.cleanup_color, 2, cv2.LINE_4)

        # Live Peaks
        for p_time, p_val in frame_data['live_peaks']:
            px, py = self._get_ax_coords('ax_transient', p_time, p_val, dynamic_xlim=xlim)
            if meta['bbox_px'][0] <= px <= meta['bbox_px'][2]:
                cv2.drawMarker(frame, (px, py), self.highlight_color, cv2.MARKER_TILTED_CROSS, 10, 2)

    def _render_snapshot_shapes(self, frame, frame_data, static_data):
        meta = self.meta['ax_snapshot']
        scores = frame_data['rolling_window_scores']
        if not scores: return

        latest_p_frame = max(s['frame'] for s in scores)
        for s in scores:
            rel_ms = float(s['frame'] - latest_p_frame)
            px, py = self._get_ax_coords('ax_snapshot', rel_ms, s['band_idx'])
            cv2.line(frame, (px, py - 20), (px, py + 20), self.band_colors[s['band_idx']], 3, cv2.LINE_4)

    def _render_buffer_shapes(self, frame, frame_data, static_data):
        meta = self.meta['ax_buf']
        buf = frame_data['accumulated_buffer']

        curr_max = np.max(buf[:-99]) if len(buf) > 99 else 0.1
        ylim = (0, max(0.1, curr_max * 1.1))

        pts = []
        for i, v in enumerate(buf):
            px, py = self._get_ax_coords('ax_buf', -5000 + i, v, dynamic_ylim=ylim)
            pts.append([px, py])

        cv2.polylines(frame, [np.array(pts)], False, self.buffer_color, 2, cv2.LINE_AA)

        # Mean line
        _, my = self._get_ax_coords('ax_buf', 0, frame_data['mean'], dynamic_ylim=ylim)
        cv2.line(frame, (int(meta['bbox_px'][0]), my), (int(meta['bbox_px'][2]), my), self.mean_color, 1, cv2.LINE_4)

        # Flashes
        for flash in frame_data['active_flashes']:
            alpha = flash['alpha']
            overlay = frame.copy()
            flash_pts = []
            for i, v in enumerate(flash['snapshot']):
                 px, py = self._get_ax_coords('ax_buf', -5000 + i, v, dynamic_ylim=ylim)
                 flash_pts.append([px, py])
            flash_pts.append([int(meta['bbox_px'][2]), int(meta['bbox_px'][3])])
            flash_pts.append([int(meta['bbox_px'][0]), int(meta['bbox_px'][3])])
            cv2.fillPoly(overlay, [np.array(flash_pts)], (46, 204, 113))
            cv2.addWeighted(overlay, alpha, frame, 1 - alpha, 0, frame)

        if frame_data['highest_peak_ms'] is not None:
             hx, _ = self._get_ax_coords('ax_buf', frame_data['highest_peak_ms'], 0, dynamic_ylim=ylim)
             if meta['bbox_px'][0] <= hx <= meta['bbox_px'][2]:
                 cv2.line(frame, (hx, int(meta['bbox_px'][1])), (hx, int(meta['bbox_px'][3])), self.highlight_color, 2, cv2.LINE_4)

    def _render_text_overlays(self, draw, frame_data, static_data):
        ax1 = self.meta['ax_transient']['bbox_px']
        ax3 = self.meta['ax_buf']['bbox_px']

        # Big Score and Rating
        score_c = self.get_score_color(frame_data['current_score'], frame_data['min_score'], frame_data['max_score'])
        draw.text((ax1[0], ax1[1] - 40), f"Score: {frame_data['current_score']:+.2f}", fill=score_c, font=self.fonts['score_big'])
        draw.text((ax1[0], ax1[1] + 10), f"Rating: {frame_data['rating']:.2f}", fill=self.highlight_color, font=self.fonts['rating'])

        # Metrics
        m = frame_data['metrics']
        metrics_txt = f"Std Dev: {m['std_dev']:.3f}\nContrast: {m['contrast']:.3f}\nPeak Std: {m['peak_std']:.3f}"
        draw.multiline_text((ax3[0] + 10, ax3[1] + 10), metrics_txt, fill=self.highlight_color, font=self.fonts['metrics'])

        # Debug Console
        for i, line in enumerate(frame_data['debug_lines']):
             color = self.band_colors[line['band_idx']]
             a = line['alpha']
             c = tuple(int(channel * a + 255 * (1-a)) for channel in color)
             draw.text((ax1[2] - 10, ax1[1] + 10 + i*20), line['text'], fill=c, font=self.fonts['debug'], anchor="ra")

        # Snapshot scores
        scores = frame_data['rolling_window_scores']
        if scores:
            latest_p_frame = max(s['frame'] for s in scores)
            for s in scores:
                rel_ms = float(s['frame'] - latest_p_frame)
                px, py = self._get_ax_coords('ax_snapshot', rel_ms, s['band_idx'])
                score_c = self.get_score_color(s['score'], frame_data['min_score'], frame_data['max_score'])
                txt = f"{s['score']:+.2f}"
                w = draw.textlength(txt, font=self.fonts['snapshot_score'])
                draw.text((px - w - 8, py - 8), txt, fill=score_c, font=self.fonts['snapshot_score'])

        # Popup Scores (Transient Plot)
        curr_time = static_data['times'][frame_data['frame']]
        xlim = (curr_time - 20, curr_time + 5)
        for s in frame_data['active_scores']:
            px, py = self._get_ax_coords('ax_transient', s['time'], s['y'], dynamic_xlim=xlim)
            if ax1[0] <= px <= ax1[2]:
                color = self.get_score_color(s['val'], frame_data['min_score'], frame_data['max_score'])
                a = s['alpha']
                c = tuple(int(channel * a + 255 * (1-a)) for channel in color)
                draw.text((px, py), f"{s['val']:+.2f}", fill=c, font=self.fonts['popup_score'])

        # Qualifiers (Buffer Plot)
        buf_buf = frame_data['accumulated_buffer']
        curr_max = np.max(buf_buf[:-99]) if len(buf_buf) > 99 else 0.1
        ylim_buf = (0, max(0.1, curr_max * 1.1))
        for q in frame_data['active_qualifiers']:
            qx, qy = self._get_ax_coords('ax_buf', q['ms'], q['val'], dynamic_ylim=ylim_buf)
            if ax3[0] <= qx <= ax3[2]:
                color = self.get_score_color(q['val'], -1.0, 1.0)
                a = q['alpha']
                c = tuple(int(channel * a + 255 * (1-a)) for channel in color)
                draw.line([qx, ax3[1], qx, ax3[3]], fill=c, width=1)
                draw.text((qx, qy), f"{q['val']:+.2f}", fill=c, font=self.fonts['metrics'])

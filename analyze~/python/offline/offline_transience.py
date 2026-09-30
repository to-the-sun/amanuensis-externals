import numpy as np


def get_default_tolerance():
    try:
        import ct_utils
        ct_utils.ensure_extension_built()
        import cumulative_transience as ct
        return float(ct.TransientAnalyzer().tolerance)
    except Exception:
        return 9.0


def compute_offline_segment_transience(all_peaks_flat, total_duration_ms, bar_length_ms, onset_envs=None, tolerance_ms=None):
    """
    Computes segment-based cumulative transience ratings for offline audio analysis.

    Peak detection and scoring ('total_score') are generated directly by the real-time
    streaming cumulative_transience algorithm (Pass 2, with window_ms = bar_length_ms).
    Segments are used exclusively for grouping and calculating per-segment average ratings
    and for display in visualization reports.

    Returns:
      segments_data: list of dicts, one for each segment from 0 to num_segments - 1.
        Each dict contains:
          'segment_index': int
          'start_ms': float
          'end_ms': float
          'rating': float (segment average rating)
          'peaks': list of peak dicts with streaming 'total_score' and 'qualifiers'
          'cum_history': list of float (downsampled segment envelope slice for display)
    """
    if tolerance_ms is None:
        tolerance_ms = get_default_tolerance()

    if bar_length_ms <= 0:
        bar_length_ms = 1000.0

    num_segments = int(np.ceil(total_duration_ms / bar_length_ms))
    if num_segments < 1:
        num_segments = 1

    # Group peaks by segment
    segment_peaks = {i: [] for i in range(num_segments)}
    for p in all_peaks_flat:
        time_ms = p['time'] * 1000.0 if 'time' in p else p.get('time_ms', 0.0)
        seg_i = int(time_ms // bar_length_ms)
        if seg_i < 0:
            seg_i = 0
        elif seg_i >= num_segments:
            seg_i = num_segments - 1

        p_copy = dict(p)
        p_copy['time_ms'] = time_ms
        segment_peaks[seg_i].append(p_copy)

    num_frames = len(onset_envs[0]) if (onset_envs and len(onset_envs) > 0) else 0

    segments_data = []

    for seg_i in range(num_segments):
        start_ms = seg_i * bar_length_ms
        end_ms = min(total_duration_ms, (seg_i + 1) * bar_length_ms)
        curr_peaks = segment_peaks[seg_i]

        seg_scores = [p['total_score'] for p in curr_peaks]
        seg_rating = float(np.mean(seg_scores)) if seg_scores else 0.0

        # Downsample segment envelope slice for visualization display in HTML report
        start_frame = int(round(start_ms))
        end_frame = min(num_frames, int(round(end_ms)))
        if onset_envs and num_frames > 0 and start_frame < end_frame:
            seg_slice = np.zeros(end_frame - start_frame, dtype=np.float64)
            for b_env in onset_envs:
                if start_frame < len(b_env):
                    band_slice = b_env[start_frame:min(end_frame, len(b_env))]
                    seg_slice[:len(band_slice)] += band_slice.astype(np.float64)
            if len(seg_slice) > 0:
                cum_hist_pts = np.interp(np.linspace(0, 1, 300), np.linspace(0, 1, len(seg_slice)), seg_slice).tolist()
            else:
                cum_hist_pts = [0.0] * 300
        else:
            cum_hist_pts = [0.0] * 300

        segments_data.append({
            'segment_index': seg_i,
            'start_ms': start_ms,
            'end_ms': end_ms,
            'rating': seg_rating,
            'peaks': curr_peaks,
            'cum_history': cum_hist_pts
        })

    return segments_data

import numpy as np


def compute_offline_segment_transience(all_peaks_flat, total_duration_ms, bar_length_ms, onset_envs=None, tolerance_ms=9.0):
    """
    Computes segment-based cumulative transience scoring for offline audio analysis.

    For each segment k:
      1. The background accumulated buffer for segment k is derived directly from the 4-band segment-length
         flux envelopes of the previous segment k-1 (if present) and the next segment k+1 (if present).
      2. Detected peaks in segment k are scored against this accumulated buffer relative to the background midpoint.
      3. All peak scores in segment k are averaged together to produce a single `rating` for the segment.

    Returns:
      segments_data: list of dicts, one for each segment from 0 to num_segments - 1.
        Each dict contains:
          'segment_index': int
          'start_ms': float
          'end_ms': float
          'rating': float (segment average rating)
          'peaks': list of peak dicts with computed 'total_score' and 'qualifiers'
          'cum_history': list of float (downsampled buffer for display)
    """
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

    # Extract segment envelope slices per band if onset_envs is provided
    # onset_envs is a list of 4 float arrays, frame duration = 1 ms
    segment_envs = []
    num_frames = len(onset_envs[0]) if (onset_envs and len(onset_envs) > 0) else 0

    lookahead_offset_frames = 200  # 200 ms lookahead offset alignment for display/scoring sync

    for seg_i in range(num_segments):
        start_frame = int(round(seg_i * bar_length_ms)) + lookahead_offset_frames
        end_frame = min(num_frames, int(round((seg_i + 1) * bar_length_ms)) + lookahead_offset_frames)
        if onset_envs and num_frames > 0 and start_frame < num_frames:
            seg_slice = np.zeros(max(1, end_frame - start_frame), dtype=np.float64)
            for b_env in onset_envs:
                if start_frame < len(b_env):
                    band_slice = b_env[start_frame:min(end_frame, len(b_env))]
                    seg_slice[:len(band_slice)] += band_slice.astype(np.float64)
            segment_envs.append(seg_slice)
        else:
            segment_envs.append(None)

    segments_data = []

    for seg_i in range(num_segments):
        start_ms = seg_i * bar_length_ms
        end_ms = min(total_duration_ms, (seg_i + 1) * bar_length_ms)
        curr_peaks = segment_peaks[seg_i]

        # Target length for segment k
        target_len = int(round(bar_length_ms))
        if target_len < 1:
            target_len = 1

        # Background accumulated buffer derived directly from segment k-1 and segment k+1 envelopes
        acc_buf = np.zeros(target_len, dtype=np.float64)
        has_neighbors = False

        for neighbor_i in (seg_i - 1, seg_i + 1):
            if 0 <= neighbor_i < num_segments and segment_envs[neighbor_i] is not None:
                n_env = segment_envs[neighbor_i]
                if len(n_env) > 0:
                    has_neighbors = True
                    if len(n_env) == target_len:
                        acc_buf += n_env
                    else:
                        old_indices = np.linspace(0, 1, len(n_env))
                        new_indices = np.linspace(0, 1, target_len)
                        acc_buf += np.interp(new_indices, old_indices, n_env)

        if not curr_peaks:
            cum_hist_pts = np.interp(np.linspace(0, 1, 300), np.linspace(0, 1, len(acc_buf)), acc_buf).tolist() if len(acc_buf) > 0 else [0.0] * 300
            segments_data.append({
                'segment_index': seg_i,
                'start_ms': start_ms,
                'end_ms': end_ms,
                'rating': 0.0,
                'peaks': [],
                'cum_history': cum_hist_pts
            })
            continue

        if not has_neighbors:
            for p in curr_peaks:
                p['total_score'] = 0.0
                p['qualifiers'] = []
            cum_hist_pts = [0.0] * 300
            segments_data.append({
                'segment_index': seg_i,
                'start_ms': start_ms,
                'end_ms': end_ms,
                'rating': 0.0,
                'peaks': curr_peaks,
                'cum_history': cum_hist_pts
            })
            continue

        midpoint = float(np.mean(acc_buf))
        max_v = float(np.max(acc_buf))
        min_v = float(np.min(acc_buf))

        tol_frames = int(round(tolerance_ms))

        for p in curr_peaks:
            p_rel_ms = p['time_ms'] - start_ms
            p_idx = int(round(p_rel_ms))
            p_idx = max(0, min(target_len - 1, p_idx))

            low = max(0, p_idx - tol_frames)
            high = min(target_len, p_idx + tol_frames + 1)

            if high > low:
                sub_buf = acc_buf[low:high]
                val = float(np.max(sub_buf))

                q = 0.0
                if val > midpoint and max_v > midpoint:
                    q = (val - midpoint) / (max_v - midpoint)
                elif val < midpoint and midpoint > min_v:
                    q = (val - midpoint) / (midpoint - min_v)

                p['total_score'] = float(p.get('peak_val', 1.0) * q)
                p['qualifiers'] = [{
                    'ms': float(p_rel_ms),
                    'orig_ms': float(p_rel_ms),
                    'val': float(q)
                }]
            else:
                p['total_score'] = 0.0
                p['qualifiers'] = []

        seg_scores = [p['total_score'] for p in curr_peaks]
        seg_rating = float(np.mean(seg_scores)) if seg_scores else 0.0

        cum_hist_pts = np.interp(np.linspace(0, 1, 300), np.linspace(0, 1, len(acc_buf)), acc_buf).tolist()

        segments_data.append({
            'segment_index': seg_i,
            'start_ms': start_ms,
            'end_ms': end_ms,
            'rating': seg_rating,
            'peaks': curr_peaks,
            'cum_history': cum_hist_pts
        })

    return segments_data

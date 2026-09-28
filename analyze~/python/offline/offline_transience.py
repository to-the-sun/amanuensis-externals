import numpy as np


def compute_offline_segment_transience(all_peaks_flat, total_duration_ms, bar_length_ms, tolerance_ms=9.0):
    """
    Computes segment-based cumulative transience scoring for offline audio analysis.

    For each segment k:
      1. The cumulative transience buffer for segment k is populated from the 4-band segment-length
         snapshots of segment k-1 (if present) and segment k+1 (if present) — yielding up to 8
         snapshots (4 bands x 2 adjacent segments).
      2. For all detected peaks in segment k, scores are derived against this accumulated buffer.
      3. All peak scores in segment k are averaged together at the same time to produce a
         single `rating` for the segment.

    Returns:
      segments_data: list of dicts, one for each segment from 0 to num_segments - 1.
        Each dict contains:
          'segment_index': int
          'start_ms': float
          'end_ms': float
          'rating': float (segment average rating)
          'peaks': list of peak dicts with computed 'total_score' and 'qualifiers'
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

    segments_data = []

    for seg_i in range(num_segments):
        start_ms = seg_i * bar_length_ms
        end_ms = min(total_duration_ms, (seg_i + 1) * bar_length_ms)

        # Gather representative peak snapshots per frequency band from prior (seg_i - 1) and next (seg_i + 1) segments
        # Up to 4 band snapshots from seg_i - 1 and 4 band snapshots from seg_i + 1 (total up to 8 snapshots)
        acc_peaks = []
        for neighbor_seg_idx in (seg_i - 1, seg_i + 1):
            if 0 <= neighbor_seg_idx < num_segments and segment_peaks[neighbor_seg_idx]:
                peaks_by_band = {}
                for pk in segment_peaks[neighbor_seg_idx]:
                    b_idx = pk.get('band_idx', 0)
                    if b_idx not in peaks_by_band or pk.get('peak_val', 0.0) > peaks_by_band[b_idx].get('peak_val', 0.0):
                        peaks_by_band[b_idx] = pk
                acc_peaks.extend(peaks_by_band.values())

        curr_peaks = segment_peaks[seg_i]

        if not curr_peaks:
            segments_data.append({
                'segment_index': seg_i,
                'start_ms': start_ms,
                'end_ms': end_ms,
                'rating': 0.0,
                'peaks': []
            })
            continue

        if not acc_peaks:
            # No neighboring peaks in segment before or after
            for p in curr_peaks:
                p['total_score'] = 0.0
                p['qualifiers'] = []
            segments_data.append({
                'segment_index': seg_i,
                'start_ms': start_ms,
                'end_ms': end_ms,
                'rating': 0.0,
                'peaks': curr_peaks
            })
            continue

        tol_idx = int(round(tolerance_ms))
        seg_len_ms = int(round(bar_length_ms))
        if seg_len_ms < 1:
            seg_len_ms = 1

        for p in curr_peaks:
            p_frame = int(round(p['time_ms']))

            # 30001-element accumulator buffer centered at p_frame (index 15000 is offset 0 ms)
            acc_buf = np.zeros(30001, dtype=np.float64)
            min_written_idx = 30001
            max_written_idx = -1

            # Add segment-length snapshots per band from segment k-1 and k+1
            for s in acc_peaks:
                s_frame = int(round(s['time_ms']))
                shift = p_frame - s_frame
                s_snap = s.get('snapshot', None)
                if s_snap is None or len(s_snap) == 0:
                    continue

                snap_len = len(s_snap)  # 15001
                start_snap_idx = max(0, 15000 - seg_len_ms)
                end_snap_idx = min(snap_len, 15001)

                for snap_idx in range(start_snap_idx, end_snap_idx):
                    buf_idx = snap_idx - shift
                    if 0 <= buf_idx < 30001:
                        acc_buf[buf_idx] += float(s_snap[snap_idx])
                        if buf_idx < min_written_idx:
                            min_written_idx = buf_idx
                        if buf_idx > max_written_idx:
                            max_written_idx = buf_idx

            if max_written_idx < min_written_idx:
                p['total_score'] = 0.0
                p['qualifiers'] = []
                continue

            # Calculate statistics across all values in the active snapshot region (including zeros)
            active_slice = acc_buf[min_written_idx : max_written_idx + 1]
            midpoint = float(np.mean(active_slice))
            max_v = float(np.max(active_slice))
            min_v = float(np.min(active_slice))

            q_sum = 0.0
            qualifiers = []

            for s in acc_peaks:
                s_frame = int(round(s['time_ms']))
                sp_idx = 15000 - (p_frame - s_frame)

                low = max(0, sp_idx - tol_idx)
                high = min(30000, sp_idx + tol_idx + 1)

                if high > low:
                    sub_buf = acc_buf[low:high]
                    best_sub = int(np.argmax(sub_buf))
                    snap_idx = low + best_sub
                    val = float(acc_buf[snap_idx])

                    q = 0.0
                    if val > midpoint and max_v > midpoint:
                        q = (val - midpoint) / (max_v - midpoint)
                    elif val < midpoint and midpoint > min_v:
                        q = (val - midpoint) / (midpoint - min_v)

                    q_sum += q
                    qualifiers.append({
                        'ms': float(snap_idx - 15000),
                        'orig_ms': float(s_frame - p_frame),
                        'val': float(q)
                    })

            p['total_score'] = float(p.get('peak_val', 1.0) * q_sum)
            p['qualifiers'] = qualifiers

        seg_scores = [p['total_score'] for p in curr_peaks]
        seg_rating = float(np.mean(seg_scores)) if seg_scores else 0.0

        # Compute cumulative history buffer across segment duration (300 points)
        num_cum_pts = 300
        cum_history = [0.0] * num_cum_pts
        if acc_peaks:
            for pt_i in range(num_cum_pts):
                t_ms = start_ms + (pt_i / float(num_cum_pts - 1 if num_cum_pts > 1 else 1)) * (end_ms - start_ms)
                val = 0.0
                for s in acc_peaks:
                    s_frame = int(round(s['time_ms']))
                    snap_idx = 15000 + int(round(t_ms - s_frame))
                    s_snap = s.get('snapshot', None)
                    if s_snap is not None and 0 <= snap_idx < len(s_snap):
                        val += float(s_snap[snap_idx])
                cum_history[pt_i] = val

        segments_data.append({
            'segment_index': seg_i,
            'start_ms': start_ms,
            'end_ms': end_ms,
            'rating': seg_rating,
            'peaks': curr_peaks,
            'cum_history': cum_history
        })

    return segments_data

import numpy as np


def compute_offline_segment_transience(all_peaks_flat, total_duration_ms, bar_length_ms, tolerance_ms=9.0):
    """
    Computes segment-based cumulative transience scoring for offline audio analysis.

    For each segment k:
      1. The cumulative transience buffer for segment k is populated strictly from the peak
         snapshots of segment k-1 (if present) and segment k+1 (if present).
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

        # Gather peak snapshots from segment before (seg_i - 1) and segment after (seg_i + 1)
        acc_peaks = []
        if seg_i > 0:
            acc_peaks.extend(segment_peaks[seg_i - 1])
        if seg_i < num_segments - 1:
            acc_peaks.extend(segment_peaks[seg_i + 1])

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

        for p in curr_peaks:
            p_frame = int(round(p['time_ms']))

            # 30001-element accumulator buffer centered at p_frame (index 15000 is offset 0 ms)
            acc_buf = np.zeros(30001, dtype=np.float64)

            # Add peak snapshots from segment k-1 and segment k+1 to acc_buf
            for s in acc_peaks:
                s_frame = int(round(s['time_ms']))
                shift = p_frame - s_frame
                s_snap = s.get('snapshot', None)
                if s_snap is None or len(s_snap) == 0:
                    continue

                snap_len = len(s_snap)  # 15001
                # Target index for snapshot element i: i - shift
                i_start = max(0, shift)
                i_end = min(snap_len, 30001 + shift)

                if i_start < i_end:
                    buf_start = i_start - shift
                    buf_end = i_end - shift
                    acc_buf[buf_start:buf_end] += s_snap[i_start:i_end]

            # Calculate statistics across non-zero regions of acc_buf
            non_zero_mask = acc_buf != 0
            if not np.any(non_zero_mask):
                p['total_score'] = 0.0
                p['qualifiers'] = []
                continue

            non_zero_vals = acc_buf[non_zero_mask]
            midpoint = float(np.mean(non_zero_vals))
            max_v = float(np.max(non_zero_vals))
            min_v = float(np.min(non_zero_vals))

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

        segments_data.append({
            'segment_index': seg_i,
            'start_ms': start_ms,
            'end_ms': end_ms,
            'rating': seg_rating,
            'peaks': curr_peaks
        })

    return segments_data

import numpy as np
cimport numpy as cnp
from libc.string cimport memcpy

cnp.import_array()

cdef extern from "cumulative_transience.h":
    ctypedef struct Qualifier:
        double ms
        double val

    ctypedef struct PeakResult:
        int p_idx
        int band_idx
        double time
        double peak_val
        double total_score
        double detected_peak_val
        double thresh_val
        double left_min
        double right_min
        double prominence
        int num_qualifiers
        Qualifier qualifiers[256]
        double snapshot[5001]

    ctypedef struct AnalyzerMetrics:
        double std_dev
        double mean
        double contrast
        double peak_std
        double rating
        bint buffer_updated
        double highest_peak_ms
        bint highest_peak_valid
        double min_score_seen
        double max_score_seen

    ctypedef struct PeakResultList:
        PeakResult peaks[64]
        int num_peaks

    ctypedef struct ChunkAnalysisResult:
        PeakResultList peak_list
        AnalyzerMetrics metrics
        float last_flux[4][100]

    ctypedef struct TransientAnalyzer_c "TransientAnalyzer":
        pass

    TransientAnalyzer_c* analyzer_create(double max_peak_value)
    void analyzer_destroy(TransientAnalyzer_c* self)
    void analyzer_set_sample_rate(TransientAnalyzer_c* self, int sr)
    double analyzer_get_max_peak(TransientAnalyzer_c* self)
    int analyzer_process_peak(TransientAnalyzer_c* self,
                              int p_idx,
                              int band_idx,
                              double time,
                              const float* env_ptr,
                              int env_len,
                              const int* all_valid_peak_indices,
                              int all_valid_count,
                              double detected_peak_val,
                              double thresh_val,
                              double left_min,
                              double right_min,
                              double prominence,
                              PeakResult* result_out)
    void analyzer_update_metrics(TransientAnalyzer_c* self, int frame, AnalyzerMetrics* metrics_out)
    double* analyzer_get_buffer(TransientAnalyzer_c* self)

    int analyzer_analyze_chunk(TransientAnalyzer_c* self,
                               const float* y,
                               int len,
                               int sr,
                               int buffer_start_frame,
                               int active_start_frame,
                               ChunkAnalysisResult* result_out)

    void analyzer_push_audio(TransientAnalyzer_c* self, const float* y, int len, int sr)

    ctypedef struct BandAnalysis:
        float* envelope
        float* rolling_threshold
        PeakResult* peaks
        int num_peaks

    ctypedef struct FullAnalysisResult:
        float* times
        int num_frames
        float max_peak_value
        BandAnalysis bands[4]
        double* ratings
        double* std_devs
        double* means
        double* contrasts
        double* peak_stds
        double min_score_seen
        double max_score_seen

    int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out)
    void analyzer_free_analysis(FullAnalysisResult* result)

cdef class TransientAnalyzer:
    cdef TransientAnalyzer_c* _c_analyzer
    cdef public double min_score_seen
    cdef public double max_score_seen
    cdef public double max_peak
    cdef object _processed_peaks

    def __cinit__(self, double max_peak_value=1.0, int sr=44100):
        self._c_analyzer = analyzer_create(max_peak_value)
        if self._c_analyzer == NULL:
            raise MemoryError()
        analyzer_set_sample_rate(self._c_analyzer, sr)
        self.min_score_seen = 0.0
        self.max_score_seen = 0.0
        self._processed_peaks = [set() for _ in range(4)]

    def __dealloc__(self):
        if self._c_analyzer != NULL:
            analyzer_destroy(self._c_analyzer)

    @property
    def accumulated_buffer(self):
        cdef double* buf_ptr = analyzer_get_buffer(self._c_analyzer)
        cdef cnp.ndarray[double, ndim=1] res = np.zeros(5001, dtype=np.float64)
        memcpy(res.data, buf_ptr, 5001 * sizeof(double))
        return res

    def process_new_peaks(self, int frame, list peak_indices_list, list onset_envs, object all_valid_peak_indices, object times, list peak_params_list=None):
        cdef list new_peaks_to_proc = []
        cdef int band_idx, p_idx
        for band_idx in range(4):
            for i, p_idx in enumerate(peak_indices_list[band_idx]):
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in self._processed_peaks[band_idx]:
                    params = None
                    if peak_params_list and band_idx < len(peak_params_list):
                        p_params = peak_params_list[band_idx]
                        if i < len(p_params['thresh_vals']):
                            params = (
                                p_params['thresh_vals'][i],
                                p_params['left_mins'][i],
                                p_params['right_mins'][i],
                                p_params['proms'][i],
                                p_params['detected_peak_vals'][i]
                            )
                    new_peaks_to_proc.append((p_idx, band_idx, params))

        new_peaks_to_proc.sort()

        cdef list results = []
        cdef list sorted_valid = sorted(list(all_valid_peak_indices))
        cdef int all_valid_count = len(sorted_valid)
        cdef cnp.ndarray[int, ndim=1] all_valid_arr = np.array(sorted_valid, dtype=np.int32)

        cdef PeakResult res
        cdef cnp.ndarray[float, ndim=1] env
        cdef int ret

        cdef double t_val, l_val, r_val, pr_val, p_val
        for p_idx, band_idx, params in new_peaks_to_proc:
            self._processed_peaks[band_idx].add(p_idx)
            env = np.ascontiguousarray(onset_envs[band_idx], dtype=np.float32)

            p_val = 0.0
            t_val = 0.0
            l_val = 0.0
            r_val = 0.0
            pr_val = 0.0
            if params:
                t_val, l_val, r_val, pr_val, p_val = params

            ret = analyzer_process_peak(
                self._c_analyzer, p_idx, band_idx, <double>times[p_idx],
                <float*>env.data, len(env),
                <int*>all_valid_arr.data, all_valid_count,
                p_val, t_val, l_val, r_val, pr_val, &res
            )

            if ret:
                peak_res = {
                    'p_idx': res.p_idx,
                    'band_idx': res.band_idx,
                    'time': res.time,
                    'peak_val': res.peak_val,
                    'total_score': res.total_score,
                    'detected_peak_val': res.detected_peak_val,
                    'thresh_val': res.thresh_val,
                    'left_min': res.left_min,
                    'right_min': res.right_min,
                    'prominence': res.prominence,
                    'qualifiers': [],
                    'snapshot': np.zeros(5001, dtype=np.float64)
                }
                for i in range(res.num_qualifiers):
                    peak_res['qualifiers'].append({'ms': res.qualifiers[i].ms, 'val': res.qualifiers[i].val})

                memcpy(cnp.PyArray_DATA(peak_res['snapshot']), res.snapshot, 5001 * sizeof(double))

                results.append(peak_res)

        return results

    def push_audio(self, cnp.ndarray[float, ndim=1] y, int sr):
        analyzer_push_audio(self._c_analyzer, <float*>y.data, len(y), sr)

    def analyze_chunk(self, cnp.ndarray[float, ndim=1] y, int sr, int buffer_start_frame, int active_start_frame):
        cdef ChunkAnalysisResult res
        cdef int ret = analyzer_analyze_chunk(self._c_analyzer, <float*>y.data, len(y), sr, buffer_start_frame, active_start_frame, &res)

        if not ret:
            return None

        cdef list peaks = []
        cdef PeakResult pr
        for i in range(res.peak_list.num_peaks):
            pr = res.peak_list.peaks[i]
            peak_data = {
                'p_idx': pr.p_idx,
                'band_idx': pr.band_idx,
                'time': pr.time,
                'peak_val': pr.peak_val,
                'total_score': pr.total_score,
                'detected_peak_val': pr.detected_peak_val,
                'thresh_val': pr.thresh_val,
                'left_min': pr.left_min,
                'right_min': pr.right_min,
                'prominence': pr.prominence,
                'qualifiers': [],
                'snapshot': np.zeros(5001, dtype=np.float64)
            }
            for j in range(pr.num_qualifiers):
                peak_data['qualifiers'].append({'ms': pr.qualifiers[j].ms, 'val': pr.qualifiers[j].val})

            memcpy(cnp.PyArray_DATA(peak_data['snapshot']), pr.snapshot, 5001 * sizeof(double))
            peaks.append(peak_data)

        m = res.metrics
        self.min_score_seen = m.min_score_seen
        self.max_score_seen = m.max_score_seen
        self.max_peak = analyzer_get_max_peak(self._c_analyzer)

        cdef list flux_list = []
        cdef cnp.ndarray[float, ndim=1] f_arr
        for b in range(4):
            f_arr = np.zeros(100, dtype=np.float32)
            memcpy(f_arr.data, res.last_flux[b], 100 * sizeof(float))
            flux_list.append(f_arr)

        return {
            'peaks': peaks,
            'flux': flux_list,
            'metrics': {
                'std_dev': m.std_dev,
                'mean': m.mean,
                'contrast': m.contrast,
                'peak_std': m.peak_std,
                'rating': m.rating,
                'buffer_updated': m.buffer_updated,
                'highest_peak_ms': m.highest_peak_ms if m.highest_peak_valid else None,
                'min_score_seen': m.min_score_seen,
                'max_score_seen': m.max_score_seen
            }
        }

    def update_metrics(self, int frame):
        cdef AnalyzerMetrics m
        analyzer_update_metrics(self._c_analyzer, frame, &m)
        self.min_score_seen = m.min_score_seen
        self.max_score_seen = m.max_score_seen

        return {
            'std_dev': m.std_dev,
            'mean': m.mean,
            'contrast': m.contrast,
            'peak_std': m.peak_std,
            'rating': m.rating,
            'buffer_updated': m.buffer_updated,
            'highest_peak_ms': m.highest_peak_ms if m.highest_peak_valid else None,
            'min_score_seen': m.min_score_seen,
            'max_score_seen': m.max_score_seen
        }

def analyze_audio(cnp.ndarray[float, ndim=1] y, int sr):
    cdef FullAnalysisResult res
    cdef int ret = analyzer_batch_analyze(<float*>y.data, len(y), sr, &res)

    if not ret:
        return None

    cdef int num_frames = res.num_frames
    cdef cnp.ndarray[float, ndim=1] times = np.zeros(num_frames, dtype=np.float32)
    memcpy(times.data, res.times, num_frames * sizeof(float))

    cdef list onset_envs = []
    cdef list rolling_thresholds = []
    cdef list full_peaks_list = []

    cdef cnp.ndarray[float, ndim=1] env
    cdef cnp.ndarray[float, ndim=1] thresh
    cdef PeakResult pr

    for i in range(4):
        env = np.zeros(num_frames, dtype=np.float32)
        memcpy(env.data, res.bands[i].envelope, num_frames * sizeof(float))
        onset_envs.append(env)

        thresh = np.zeros(num_frames, dtype=np.float32)
        memcpy(thresh.data, res.bands[i].rolling_threshold, num_frames * sizeof(float))
        rolling_thresholds.append(thresh)

        band_peaks = []
        for k in range(res.bands[i].num_peaks):
            pr = res.bands[i].peaks[k]
            peak_data = {
                'p_idx': pr.p_idx,
                'band_idx': pr.band_idx,
                'time': pr.time,
                'peak_val': pr.peak_val,
                'total_score': pr.total_score,
                'detected_peak_val': pr.detected_peak_val,
                'thresh_val': pr.thresh_val,
                'left_min': pr.left_min,
                'right_min': pr.right_min,
                'prominence': pr.prominence,
                'qualifiers': [],
                'snapshot': np.zeros(5001, dtype=np.float64)
            }
            for j in range(pr.num_qualifiers):
                peak_data['qualifiers'].append({'ms': pr.qualifiers[j].ms, 'val': pr.qualifiers[j].val})
            memcpy(cnp.PyArray_DATA(peak_data['snapshot']), pr.snapshot, 5001 * sizeof(double))
            band_peaks.append(peak_data)
        full_peaks_list.append(band_peaks)

    cdef cnp.ndarray[double, ndim=1] ratings = np.zeros(num_frames, dtype=np.float64)
    memcpy(ratings.data, res.ratings, num_frames * sizeof(double))

    cdef cnp.ndarray[double, ndim=1] std_devs = np.zeros(num_frames, dtype=np.float64)
    memcpy(std_devs.data, res.std_devs, num_frames * sizeof(double))

    cdef cnp.ndarray[double, ndim=1] means = np.zeros(num_frames, dtype=np.float64)
    memcpy(means.data, res.means, num_frames * sizeof(double))

    cdef cnp.ndarray[double, ndim=1] contrasts = np.zeros(num_frames, dtype=np.float64)
    memcpy(contrasts.data, res.contrasts, num_frames * sizeof(double))

    cdef cnp.ndarray[double, ndim=1] peak_stds = np.zeros(num_frames, dtype=np.float64)
    memcpy(peak_stds.data, res.peak_stds, num_frames * sizeof(double))

    cdef float max_peak_value = res.max_peak_value
    cdef double min_score_seen = res.min_score_seen
    cdef double max_score_seen = res.max_score_seen

    analyzer_free_analysis(&res)

    return {
        "times": times,
        "sample_rate": int(sr),
        "max_peak_value": float(max_peak_value),
        "min_score_seen": float(min_score_seen),
        "max_score_seen": float(max_score_seen),
        "onset_envs": onset_envs,
        "rolling_thresholds": rolling_thresholds,
        "peaks": full_peaks_list,
        "ratings": ratings,
        "std_devs": std_devs,
        "means": means,
        "contrasts": contrasts,
        "peak_stds": peak_stds
    }

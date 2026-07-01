import numpy as np
import librosa
import sys
import os

# Add the extension path to the START of sys.path
extension_path = os.path.abspath("analyze~/python")
if extension_path in sys.path: sys.path.remove(extension_path)
sys.path.insert(0, extension_path)

import cumulative_transience as ct
print(f"Loaded extension from: {ct.__file__}")

def main():
    audio_path = "./sounds~/design/sounds/1/design_output.wav"
    if not os.path.exists(audio_path):
        print(f"Error: {audio_path} not found.")
        return

    print(f"Loading {audio_path}...")
    y, sr = librosa.load(audio_path, sr=44100, mono=True)

    # 1. Batch Analysis
    print("Running Batch Analysis...")
    batch_res = ct.analyze_audio(y, sr)
    batch_maxes = [np.max(env) for env in batch_res['onset_envs']]

    # 2. Incremental Analysis
    print("Running Incremental Analysis...")
    analyzer = ct.TransientAnalyzer(max_peak_value=1.0, sr=sr)

    hop_samples_ms = 44
    step_samples = int(sr * 0.1)
    inc_flux_maxes = [0.0] * 4

    last_t = 0
    all_inc_peaks = []

    for t_samples in range(step_samples, len(y) + step_samples, step_samples):
        hop_y = y[last_t : t_samples]
        last_t = t_samples

        active_start_samples = t_samples - step_samples - int(sr * 0.2)
        if active_start_samples < 0:
            analyzer.push_audio(hop_y, sr)
            continue

        window_start_samples = active_start_samples - int(sr * 15.0)
        if window_start_samples < 0: window_start_samples = 0

        buffer_start_frame = window_start_samples // hop_samples_ms
        active_start_frame = active_start_samples // hop_samples_ms

        res = analyzer.analyze_chunk(hop_y, sr, buffer_start_frame, active_start_frame)
        if res:
            for p in res['peaks']:
                all_inc_peaks.append(p)
                b = p['band_idx']
                if p['detected_peak_val'] > inc_flux_maxes[b]:
                    inc_flux_maxes[b] = p['detected_peak_val']

    print("\n--- RESULTS ---")
    print(f"Batch Max Flux per band: {batch_maxes}")
    print(f"Incremental Max Flux (from detected peaks) per band: {inc_flux_maxes}")

    # Compare peaks
    for b in range(4):
        batch_peaks = batch_res['peaks_list'][b]
        batch_peak_vals = [batch_res['onset_envs'][b][idx] for idx in batch_peaks]
        batch_peak_max = max(batch_peak_vals) if batch_peak_vals else 0

        print(f"\nBand {b}:")
        print(f"  Batch Peak Max: {batch_peak_max:.4f}")
        print(f"  Incremental Peak Max: {inc_flux_maxes[b]:.4f}")
        if batch_peak_max > 0:
            ratio = inc_flux_maxes[b] / batch_peak_max
            print(f"  Ratio (Inc/Batch): {ratio:.4f}")

if __name__ == "__main__":
    main()

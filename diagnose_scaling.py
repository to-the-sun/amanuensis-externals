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

    # 2. Incremental Analysis
    print("Running Incremental Analysis...")
    analyzer = ct.TransientAnalyzer(max_peak_value=1.0, sr=sr)

    hop_samples_ms = 44
    step_samples = int(sr * 0.1) # 4410 samples = 100 frames

    all_inc_flux = [[] for _ in range(4)]

    last_t = 0
    for t_samples in range(step_samples, len(y) + step_samples, step_samples):
        hop_y = y[last_t : t_samples]
        last_t = t_samples

        if len(hop_y) == 0: break

        active_start_samples = t_samples - len(hop_y) - int(sr * 0.2)
        window_start_samples = active_start_samples - int(sr * 15.0)
        if window_start_samples < 0: window_start_samples = 0

        buffer_start_frame = window_start_samples // hop_samples_ms
        active_start_frame = active_start_samples // hop_samples_ms

        res = analyzer.analyze_chunk(hop_y, sr, buffer_start_frame, active_start_frame)
        if res:
            for b in range(4):
                all_inc_flux[b].extend(res['flux'][b])

    print("\n--- COMPARISON ---")
    num_compare = min(len(batch_res['onset_envs'][0]), len(all_inc_flux[0]))

    for b in range(2): # Just check first two bands
        batch_vals = np.array(batch_res['onset_envs'][b][:num_compare])
        inc_vals = np.array(all_inc_flux[b][:num_compare])

        print(f"\nBand {b}:")
        print(f"  Batch Mean: {np.mean(batch_vals):.4f}")
        print(f"  Inc Mean:   {np.mean(inc_vals):.4f}")

        # Find some non-zero index to compare
        nonzero = np.where(inc_vals > 0.1)[0]
        if len(nonzero) > 0:
            idx = nonzero[len(nonzero)//2]
            print(f"  At frame {idx}: Batch={batch_vals[idx]:.4f}, Inc={inc_vals[idx]:.4f}, Ratio={inc_vals[idx]/batch_vals[idx]:.4f}")
        else:
            print("  No significant flux found.")

if __name__ == "__main__":
    main()

import os
import numpy as np
from scipy.io import wavfile

def generate_varied_control_test_set():
    sr = 44100
    duration = 30.0
    total_samples = int(sr * duration)

    # Base rhythmic beat times: 60 notes across 30 seconds (0.5s interval / 120 BPM steady beat)
    base_beats = [i * 0.5 for i in range(60)]

    # Human timing perception limit (JND for micro-timing shifts): ~20-25 ms at 120 BPM (500ms IBI).
    # Introduce pseudo-random human timing variance per beat up to +/- 25 ms (+/- 0.025s).
    # Each beat gets a unique offset, ranging from nearly 0ms (tight) up to ~25ms (bordering on offbeat).
    np.random.seed(12345)  # Reproducible random seed
    # Random offsets between -0.025 and +0.025 seconds for each beat
    timing_offsets = np.random.uniform(-0.025, 0.025, len(base_beats))

    # Ensure beat 0 starts at >= 0.0s
    beats = [max(0.0, base_beats[i] + timing_offsets[i]) for i in range(len(base_beats))]

    def generate_sustained_bass():
        audio = np.zeros(total_samples, dtype=np.float32)
        note_dur = 0.45
        note_len = int(sr * note_dur)
        t_note = np.arange(note_len) / sr

        freq = 75.0
        sig = 0.8 * np.sin(2 * np.pi * freq * t_note) + 0.2 * np.sin(2 * np.pi * 2 * freq * t_note)

        # Envelope: Ultra-smooth 250ms S-curve attack (minimal transience), sustain, 100ms release
        env = np.ones(note_len, dtype=np.float32)
        att_samples = int(sr * 0.25)
        rel_samples = int(sr * 0.10)

        att_curve = 0.5 * (1.0 - np.cos(np.pi * np.linspace(0, 1, att_samples)))
        env[:att_samples] = att_curve

        rel_curve = 0.5 * (1.0 + np.cos(np.pi * np.linspace(0, 1, rel_samples)))
        env[-rel_samples:] = rel_curve

        note_waveform = sig * env

        for onset in beats:
            start_idx = int(onset * sr)
            end_idx = min(start_idx + note_len, total_samples)
            actual_len = end_idx - start_idx
            if actual_len > 0:
                audio[start_idx:end_idx] += note_waveform[:actual_len]

        return audio

    def generate_sustained_treble():
        audio = np.zeros(total_samples, dtype=np.float32)
        note_dur = 0.45
        note_len = int(sr * note_dur)
        t_note = np.arange(note_len) / sr

        freq = 1760.0
        sig = 0.8 * np.sin(2 * np.pi * freq * t_note) + 0.15 * np.sin(2 * np.pi * 2 * freq * t_note)

        # Envelope: Ultra-smooth 250ms S-curve attack (minimal transience), sustain, 100ms release
        env = np.ones(note_len, dtype=np.float32)
        att_samples = int(sr * 0.25)
        rel_samples = int(sr * 0.10)

        att_curve = 0.5 * (1.0 - np.cos(np.pi * np.linspace(0, 1, att_samples)))
        env[:att_samples] = att_curve

        rel_curve = 0.5 * (1.0 + np.cos(np.pi * np.linspace(0, 1, rel_samples)))
        env[-rel_samples:] = rel_curve

        note_waveform = sig * env

        for onset in beats:
            start_idx = int(onset * sr)
            end_idx = min(start_idx + note_len, total_samples)
            actual_len = end_idx - start_idx
            if actual_len > 0:
                audio[start_idx:end_idx] += note_waveform[:actual_len]

        return audio

    def generate_transient_bass():
        audio = np.zeros(total_samples, dtype=np.float32)
        note_dur = 0.25
        note_len = int(sr * note_dur)
        t_note = np.arange(note_len) / sr

        freq = 45.0 + (160.0 - 45.0) * np.exp(-t_note / 0.03)
        phase = 2 * np.pi * np.cumsum(freq) / sr
        sig = np.sin(phase)

        env = (1.0 - np.exp(-t_note / 0.001)) * np.exp(-t_note / 0.04)
        note_waveform = sig * env

        for onset in beats:
            start_idx = int(onset * sr)
            end_idx = min(start_idx + note_len, total_samples)
            actual_len = end_idx - start_idx
            if actual_len > 0:
                audio[start_idx:end_idx] += note_waveform[:actual_len]

        return audio

    def generate_transient_treble():
        audio = np.zeros(total_samples, dtype=np.float32)
        note_dur = 0.2
        note_len = int(sr * note_dur)
        t_note = np.arange(note_len) / sr

        np.random.seed(42)
        noise = np.random.uniform(-1.0, 1.0, note_len)
        sine = np.sin(2 * np.pi * 4000.0 * t_note)
        sig = 0.7 * noise + 0.3 * sine

        env = (1.0 - np.exp(-t_note / 0.0005)) * np.exp(-t_note / 0.015)
        note_waveform = sig * env

        for onset in beats:
            start_idx = int(onset * sr)
            end_idx = min(start_idx + note_len, total_samples)
            actual_len = end_idx - start_idx
            if actual_len > 0:
                audio[start_idx:end_idx] += note_waveform[:actual_len]

        return audio

    out_dir = os.path.dirname(os.path.abspath(__file__))

    files = {
        'sustained_bass_varied.wav': generate_sustained_bass(),
        'sustained_treble_varied.wav': generate_sustained_treble(),
        'transient_bass_varied.wav': generate_transient_bass(),
        'transient_treble_varied.wav': generate_transient_treble()
    }

    for name, data in files.items():
        max_val = np.max(np.abs(data))
        if max_val > 0:
            data = (data / max_val) * 0.891  # Peak normalize to -1.0 dBFS
        path = os.path.join(out_dir, name)
        wavfile.write(path, sr, (data * 32767).astype(np.int16))
        print(f"Generated {path}: duration={len(data)/sr:.2f}s")

if __name__ == '__main__':
    generate_varied_control_test_set()

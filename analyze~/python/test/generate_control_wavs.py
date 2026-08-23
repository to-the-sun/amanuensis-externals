import os
import numpy as np
from scipy.io import wavfile

def generate_control_test_set():
    sr = 44100
    duration = 30.0
    total_samples = int(sr * duration)

    # Rhythmic note onset times: 60 notes across 30 seconds (0.5s interval / 120 BPM steady beat)
    # Note #26 (index 26) is shifted off-beat from 13.0s to 13.25s
    beats = [i * 0.5 for i in range(60)]
    beats[26] = 13.25  # Off-beat note

    def generate_sustained_bass():
        audio = np.zeros(total_samples, dtype=np.float32)
        note_dur = 0.65
        note_len = int(sr * note_dur)
        t_note = np.arange(note_len) / sr

        # Low frequency fundamental (75 Hz) + soft 2nd harmonic
        freq = 75.0
        sig = 0.8 * np.sin(2 * np.pi * freq * t_note) + 0.2 * np.sin(2 * np.pi * 2 * freq * t_note)

        # Envelope: Ultra-smooth 500ms S-curve attack (half-second swell, zero transience), 150ms release
        env = np.ones(note_len, dtype=np.float32)
        att_samples = int(sr * 0.50)
        rel_samples = int(sr * 0.15)

        # Raised cosine (S-curve) attack for zero derivative at onset
        att_curve = 0.5 * (1.0 - np.cos(np.pi * np.linspace(0, 1, att_samples)))
        env[:att_samples] = att_curve

        # Smooth cosine release
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
        note_dur = 0.65
        note_len = int(sr * note_dur)
        t_note = np.arange(note_len) / sr

        # High frequency fundamental (1760 Hz / A6) + soft harmonic
        freq = 1760.0
        sig = 0.8 * np.sin(2 * np.pi * freq * t_note) + 0.15 * np.sin(2 * np.pi * 2 * freq * t_note)

        # Envelope: Ultra-smooth 500ms S-curve attack (half-second swell, zero transience), 150ms release
        env = np.ones(note_len, dtype=np.float32)
        att_samples = int(sr * 0.50)
        rel_samples = int(sr * 0.15)

        # Raised cosine (S-curve) attack for zero derivative at onset
        att_curve = 0.5 * (1.0 - np.cos(np.pi * np.linspace(0, 1, att_samples)))
        env[:att_samples] = att_curve

        # Smooth cosine release
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

        # Pitch sweep 160 Hz -> 45 Hz exponential decay (kick drum sound)
        freq = 45.0 + (160.0 - 45.0) * np.exp(-t_note / 0.03)
        phase = 2 * np.pi * np.cumsum(freq) / sr
        sig = np.sin(phase)

        # Sharp attack (1ms), fast exponential decay (tau = 0.04s)
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

        # High frequency white noise burst + 4000 Hz sine pop (hi-hat / percussive transient)
        np.random.seed(42)
        noise = np.random.uniform(-1.0, 1.0, note_len)
        sine = np.sin(2 * np.pi * 4000.0 * t_note)
        sig = 0.7 * noise + 0.3 * sine

        # Extremely fast attack (0.5ms), sharp decay (tau = 0.015s)
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
        'sustained_bass.wav': generate_sustained_bass(),
        'sustained_treble.wav': generate_sustained_treble(),
        'transient_bass.wav': generate_transient_bass(),
        'transient_treble.wav': generate_transient_treble()
    }

    for name, data in files.items():
        max_val = np.max(np.abs(data))
        if max_val > 0:
            data = (data / max_val) * 0.891  # Peak normalize to -1.0 dBFS
        path = os.path.join(out_dir, name)
        wavfile.write(path, sr, (data * 32767).astype(np.int16))
        print(f"Generated {path}: duration={len(data)/sr:.2f}s")

if __name__ == '__main__':
    generate_control_test_set()

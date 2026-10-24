"""Checks that a WAV holds a clean tone at the expected frequency: python3 check_tone.py file.wav 440"""
import sys
import wave

import numpy as np

w = wave.open(sys.argv[1])
want = float(sys.argv[2])
rate = w.getframerate()
x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, w.getnchannels())[:, 0].astype(float)
x = x[rate // 2:]  # skip codec warm-up
if len(x) < rate // 2:
    print("  too short")
    sys.exit(1)
spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
peak = np.fft.rfftfreq(len(x), 1 / rate)[spec.argmax()]
rms = np.sqrt((x ** 2).mean()) / 32768
print(f"  {len(x) / rate + 0.5:.1f}s, peak {peak:.0f} Hz, rms {rms:.2f}")
sys.exit(0 if abs(peak - want) < 5 and rms > 0.05 else 1)

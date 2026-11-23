"""
ANCHOR - make test sounds to play from a phone or laptop speaker.

Install once:  pip install numpy
Run:           python make_test_sounds.py
Makes a folder test_sounds/ with:
  tone_200Hz.wav      steady 200 Hz tone (easiest thing to cancel)
  hum_100Hz.wav       100 Hz hum with harmonics (like a motor or transformer)
  machine_like.wav    hum + rumble noise (harder)
  alarm_beeps.wav     1 kHz beeps, 0.5 s on / 0.5 s off (like a reversing alarm)
Each file is 60 s long. Play them at a FIXED volume and distance every time.
"""
import os, wave
import numpy as np

RATE = 44100
SECONDS = 60
t = np.arange(RATE * SECONDS) / RATE
rng = np.random.default_rng(1)


def save(name, x):
    x = x / (np.max(np.abs(x)) + 1e-9) * 0.7
    os.makedirs("test_sounds", exist_ok=True)
    with wave.open(os.path.join("test_sounds", name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    print("made test_sounds/" + name)


def lowpass(x, cutoff_hz):
    # simple one-pole low-pass, run twice
    a = np.exp(-2 * np.pi * cutoff_hz / RATE)
    for _ in range(2):
        y = np.empty_like(x)
        acc = 0.0
        for i in range(len(x)):
            acc = (1 - a) * x[i] + a * acc
            y[i] = acc
        x = y
    return x


save("tone_200Hz.wav", np.sin(2 * np.pi * 200 * t))
hum = sum(np.sin(2 * np.pi * 100 * k * t + k) / k for k in range(1, 6))
save("hum_100Hz.wav", hum)
rumble = lowpass(rng.standard_normal(len(t)), 400)
save("machine_like.wav", hum / np.max(np.abs(hum)) + 0.8 * rumble / np.max(np.abs(rumble)))
gate = (np.floor(t * 2) % 2 == 0).astype(float)
save("alarm_beeps.wav", np.sin(2 * np.pi * 1000 * t) * gate)

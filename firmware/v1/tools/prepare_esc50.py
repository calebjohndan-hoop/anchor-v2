"""
ANCHOR - take extra training clips from the public ESC-50 dataset.

ESC-50: https://github.com/karolpiczak/ESC-50  (licence CC BY-NC: non-commercial use, credit the authors)
1. Download the ZIP from that page and unzip it, e.g. to  C:\\data\\ESC-50-master
2. pip install numpy scipy
3. python prepare_esc50.py --esc50 C:\\data\\ESC-50-master --out dataset_public

It copies the chosen categories, converts them to 16 kHz mono 16-bit, and names
them <label>.esc50_<original>.wav so Edge Impulse reads the label from the name.
ESC-50 has no plain speech or "quiet factory" class: record those yourself.
"""
import argparse, csv, os, wave
import numpy as np
from scipy.signal import resample_poly

MAP = {
    "alarm": ["siren", "clock_alarm", "car_horn"],
    "machine": ["engine", "chainsaw", "hand_saw", "vacuum_cleaner", "washing_machine"],
}


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, ch, sw, rate = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError("only 16-bit files supported")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--esc50", required=True, help="folder that contains audio/ and meta/")
    ap.add_argument("--out", default="dataset_public")
    a = ap.parse_args()
    rows = list(csv.DictReader(open(os.path.join(a.esc50, "meta", "esc50.csv"))))
    for label, cats in MAP.items():
        os.makedirs(os.path.join(a.out, label), exist_ok=True)
        count = 0
        for r in rows:
            if r["category"] not in cats:
                continue
            x, rate = read_wav(os.path.join(a.esc50, "audio", r["filename"]))
            if rate != 16000:
                g = np.gcd(16000, rate)
                x = resample_poly(x, 16000 // g, rate // g)
            x = np.clip(x, -32768, 32767).astype("<i2")
            name = f"{label}.esc50_{os.path.splitext(r['filename'])[0]}.wav"
            with wave.open(os.path.join(a.out, label, name), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
                w.writeframes(x.tobytes())
            count += 1
        print(f"{label}: {count} clips")


if __name__ == "__main__":
    main()

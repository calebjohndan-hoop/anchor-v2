# =====================================================================
# ANCHOR - test the AI on your LAPTOP microphone (no ESP32 needed)
#
# Prints the same lines as sketch 08 in the Arduino Serial Monitor:
#   alarm 0.01   machine 0.97   other 0.01   quiet 0.00   voice 0.01   => machine
# plus the sound level and the mode sketch 09 would pick.
#
# Needs:  pip install numpy sounddevice
# Run:    python anchor_laptop_test.py                 (listens to the default microphone)
#         python anchor_laptop_test.py --list          (shows microphones and their numbers)
#         python anchor_laptop_test.py --device 2      (use microphone number 2)
#         python anchor_laptop_test.py --wav file.wav  (test a sound file instead)
#         python anchor_laptop_test.py --quiet-db -50  (change the "quiet" level)
# Stop with Ctrl+C.
#
# Same maths as anchor_ai.h on the ESP32, so the answers match what the headset
# would say for the same sound (but a laptop mic sounds different from the headset mic).
# =====================================================================
import argparse, os, sys, time, wave
import numpy as np

FS, WIN, NFFT, HOP = 16000, 16000, 512, 256
NFRAMES = 1 + (WIN - NFFT) // HOP
NMEL, FMIN, FMAX = 32, 50.0, 4000.0
TON_LO, TON_HI = 10, 128
MOD_RANGES = [(1, 2), (3, 4), (5, 8), (9, 16), (17, 30)]
LABELS = ["alarm", "machine", "other", "quiet", "voice"]   # same order as the ESP32 code


def mel_fb():
    h2m = lambda f: 2595.0 * np.log10(1.0 + f / 700.0)
    m2h = lambda m: 700.0 * (10 ** (m / 2595.0) - 1.0)
    pts = m2h(np.linspace(h2m(FMIN), h2m(FMAX), NMEL + 2))
    freqs = np.arange(NFFT // 2 + 1) * FS / NFFT
    fb = np.zeros((NMEL, NFFT // 2 + 1))
    for m in range(NMEL):
        lo, ce, hi = pts[m], pts[m + 1], pts[m + 2]
        fb[m] = np.maximum(0, np.minimum((freqs - lo) / (ce - lo), (hi - freqs) / (hi - ce)))
    return fb


FB = mel_fb()
HANN = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)


def features(x16):
    x = x16.astype(np.float64) / 32768.0
    idx = np.arange(NFFT)[None, :] + HOP * np.arange(NFRAMES)[:, None]
    p = np.abs(np.fft.rfft(x[idx] * HANN, axis=1)) ** 2
    db = 10.0 * np.log10(p @ FB.T + 1e-10)
    mean = db.mean(axis=0)
    rel = mean - mean.mean()
    std = db.std(axis=0)
    dif = np.abs(np.diff(db, axis=0)).mean(axis=0)
    peak = db.max(axis=0) - mean
    q = p[:, TON_LO:TON_HI + 1]
    am = q.mean(axis=1)
    ton = 10 * np.log10((q.max(axis=1) + 1e-12) / (am + 1e-12))
    gm = np.exp(np.log(q + 1e-12).mean(axis=1))
    flat = 10 * np.log10((gm + 1e-12) / (am + 1e-12))
    tonal = [ton.mean(), ton.std(), ton.max(), flat.mean(), flat.std(), flat.min()]
    mods, t = [], np.arange(NFRAMES)
    for g in range(4):
        e = db[:, 8 * g:8 * g + 8].mean(axis=1)
        e = e - e.mean()
        X = np.array([np.abs(np.sum(e * np.exp(-2j * np.pi * k * t / NFRAMES))) ** 2 for k in range(1, 31)])
        tot = X.sum() + 1e-9
        for lo, hi in MOD_RANGES:
            mods.append(X[lo - 1:hi].sum() / tot)
    return np.concatenate([rel, std, dif, peak, tonal, mods]).astype(np.float32)


class Model:
    def __init__(self, path):
        d = np.load(path)
        self.base, self.start, self.cls = d["base"], d["start"], d["cls"]
        self.feat, self.thr, self.left, self.right = d["feat"], d["thr"], d["left"], d["right"]

    def predict(self, f):
        raw = self.base.copy()
        for t in range(len(self.start)):
            n = self.start[t]
            while self.feat[n] >= 0:
                n = self.start[t] + (self.left[n] if f[self.feat[n]] <= self.thr[n] else self.right[n])
            raw[self.cls[t]] += self.thr[n]
        e = np.exp(raw - raw.max())
        return e / e.sum()


def classify(model, x16, quiet_db):
    level = 10 * np.log10(np.mean((x16.astype(np.float64) / 32768.0) ** 2) + 1e-12)
    if level < quiet_db:
        vals = [0, 0, 0, 1.0, 0]
    else:
        p = model.predict(features(x16))                 # alarm, machine, other, voice
        vals = [p[0], p[1], p[2], 0.0, p[3]]
    return vals, level


class Mode:
    """Simplified version of the mode logic in sketch 09 (so you can see what the headset would do)."""
    def __init__(self):
        self.mode, self.alarm_until, self.machine_run = "IDLE", 0.0, 0

    def update(self, v, now):
        a, m, o, q, vo = v
        if a >= 0.40:
            self.alarm_until = now + 3.0
            self.mode = "ALARM  (cancelling OFF, you hear it)"
            self.machine_run = 0
            return self.mode
        if now < self.alarm_until:
            return "ALARM  (holding 3 s)"
        best = int(np.argmax(v))
        if best == 3:
            self.machine_run = 0; self.mode = "IDLE   (quiet)"
        elif v[best] < 0.60 or best == 2:
            self.machine_run = 0; self.mode = "SAFE   (not sure / other: cancelling off)"
        elif best == 4:
            self.machine_run = 0; self.mode = "VOICE  (half cancel, half pass)"
        elif best == 1:
            self.machine_run += 1
            self.mode = "CANCEL (machine)" if self.machine_run >= 2 else "SAFE   (waiting for a 2nd machine answer)"
        return self.mode


def show(v, level, ms, mode):
    s = "   ".join("%s %.2f" % (LABELS[i], v[i]) for i in range(5))
    print("%s   => %-8s level %6.1f dBFS (%d ms)   mode: %s" % (s, LABELS[int(np.argmax(v))], level, ms, mode), flush=True)


def read_wav(path):
    w = wave.open(path, "rb")
    n, ch, sw, fs = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    raw = w.readframes(n); w.close()
    if sw != 2: sys.exit("Use a 16-bit WAV file.")
    x = np.frombuffer(raw, dtype="<i2").reshape(-1, ch)[:, 0].astype(np.float64)
    if fs != FS:
        x = np.interp(np.arange(0, len(x), fs / FS), np.arange(len(x)), x)
    return x.astype(np.int16)


def test_file(a, model, mode):
    x = read_wav(a.wav)
    print("File: %s  (%.1f s). Answers every %.1f s:" % (a.wav, len(x) / FS, a.hop))
    step = int(a.hop * FS)
    counts, levels = {}, []
    for o in range(0, max(1, len(x) - WIN + 1), step):
        seg = x[o:o + WIN]
        if len(seg) < WIN: seg = np.pad(seg, (0, WIN - len(seg)))
        t0 = time.time(); v, lv = classify(model, seg, a.quiet_db); ms = (time.time() - t0) * 1000
        show(v, lv, ms, mode.update(v, o / FS))
        lab = LABELS[int(np.argmax(v))]; counts[lab] = counts.get(lab, 0) + 1; levels.append(lv)
    print("\nSUMMARY: %s   loudness %.1f to %.1f dBFS" % (", ".join("%s x%d" % kv for kv in sorted(counts.items())), min(levels), max(levels)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="list microphones")
    ap.add_argument("--device", type=int, default=None, help="microphone number from --list")
    ap.add_argument("--wav", help="test a 16-bit WAV file instead of the microphone")
    ap.add_argument("--quiet-db", type=float, default=-55.0, help="softer than this = quiet (default -55)")
    ap.add_argument("--model", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "anchor_model.npz"))
    ap.add_argument("--record", type=float, default=0, help="record this many seconds to mic_test.wav, then test it")
    ap.add_argument("--hop", type=float, default=1.0, help="seconds between answers (1.0 = like sketch 08, 0.5 = like sketch 09)")
    a = ap.parse_args()
    model = Model(a.model)
    mode = Mode()

    if a.wav and a.record <= 0:
        return test_file(a, model, mode)

    try:
        import sounddevice as sd
    except Exception as e:
        sys.exit("Install the microphone library first:  pip install sounddevice\n(%s)" % e)
    if a.list:
        print(sd.query_devices()); return
    if a.record > 0:
        print("Recording %.0f s from: %s ... make the sound now" % (a.record, sd.query_devices(a.device, "input")["name"]))
        r = sd.rec(int(a.record * FS), samplerate=FS, channels=1, dtype="int16", device=a.device)
        sd.wait()
        out = os.path.join(os.getcwd(), "mic_test.wav")
        w = wave.open(out, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(FS)
        w.writeframes(r[:, 0].astype("<i2").tobytes()); w.close()
        print("Saved", out, "- send this file to check what the mic heard.\n")
        a.wav = out
        return test_file(a, model, mode)

    buf = np.zeros(WIN, dtype=np.int16)
    state = {"new": 0}

    def cb(indata, frames, tm, status):
        x = (np.clip(indata[:, 0], -1, 1) * 32767).astype(np.int16)
        if len(x) >= WIN:
            buf[:] = x[-WIN:]
        else:
            buf[:-len(x)] = buf[len(x):].copy()
            buf[-len(x):] = x
        state["new"] += len(x)

    dev = sd.query_devices(a.device, "input")
    print("ANCHOR laptop test - microphone: %s" % dev["name"])
    print("Labels: %s. Make sounds near the laptop. Ctrl+C to stop." % " ".join(LABELS))
    print("Tip: if it always says quiet, run again with  --quiet-db -65 ; if it never says quiet, use -45.")
    with sd.InputStream(device=a.device, channels=1, samplerate=FS, dtype="float32", callback=cb, blocksize=1024):
        t_start = time.time()
        while state["new"] < WIN:
            time.sleep(0.05)
        state["new"] = 0
        try:
            while True:
                while state["new"] < int(a.hop * FS):
                    time.sleep(0.02)
                state["new"] = 0
                seg = buf.copy()
                t0 = time.time(); v, lv = classify(model, seg, a.quiet_db); ms = (time.time() - t0) * 1000
                show(v, lv, ms, mode.update(v, time.time() - t_start))
        except KeyboardInterrupt:
            print("\nStopped.")


if __name__ == "__main__":
    main()

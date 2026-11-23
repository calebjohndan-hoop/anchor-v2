"""
ANCHOR - train the sound classifier ("the AI") without Edge Impulse.

What it learns:   alarm, machine, voice, other     (+ "quiet" is a loudness rule on the board)
Where sounds come from (any mix of these):
  --esc50 FOLDER   ESC-50 dataset   https://github.com/karolpiczak/ESC-50              (CC BY-NC 3.0)
  --fsdd  FOLDER   Free Spoken Digit Dataset https://github.com/Jakobovski/free-spoken-digit-dataset (CC BY-SA 4.0)
  --own   FOLDER   your own clips from record_clips.py: FOLDER/<label>/*.wav  (labels: alarm, machine, voice, other)
Output:
  anchor_ai_model.h   -> copy it next to anchor_ai.h in sketches 08 and 09

Install once:  pip install numpy scipy scikit-learn
Run, e.g.:     python train_anchor_ai.py --esc50 ESC-50-master --fsdd free-spoken-digit-dataset --own dataset
"""
import argparse, csv, glob, json, os, sys, wave
import numpy as np
from scipy.signal import resample_poly, lfilter

# ---------------- feature settings: MUST match anchor_ai.h ----------------
FS = 16000
WIN = 16000          # 1 second window
NFFT = 512           # 32 ms frames
HOP = 256            # 16 ms hop
NFRAMES = 1 + (WIN - NFFT) // HOP   # 61
NMEL = 32
FMIN, FMAX = 50.0, 4000.0           # speech data is 8 kHz, so we only look below 4 kHz
LABELS = ["alarm", "machine", "other", "voice"]   # trained classes (quiet is a loudness rule)

ESC_MAP = {
    "alarm": ["siren", "clock_alarm", "car_horn", "church_bells"],
    "machine": ["engine", "chainsaw", "hand_saw", "vacuum_cleaner", "washing_machine", "airplane", "helicopter"],
    "skip": ["train"],   # trains mix engine noise with horns: too ambiguous
}


def hz2mel(f):
    return 2595.0 * np.log10(1.0 + f / 700.0)


def mel2hz(m):
    return 700.0 * (10 ** (m / 2595.0) - 1.0)


def mel_filterbank():
    pts = mel2hz(np.linspace(hz2mel(FMIN), hz2mel(FMAX), NMEL + 2))
    freqs = np.arange(NFFT // 2 + 1) * FS / NFFT
    fb = np.zeros((NMEL, NFFT // 2 + 1))
    for m in range(NMEL):
        lo, ce, hi = pts[m], pts[m + 1], pts[m + 2]
        up = (freqs - lo) / (ce - lo)
        dn = (hi - freqs) / (hi - ce)
        fb[m] = np.maximum(0, np.minimum(up, dn))
    return fb


FB = mel_filterbank()
HANN = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)


def window_level_db(x16):
    x = x16.astype(np.float64) / 32768.0
    return 10 * np.log10(np.mean(x * x) + 1e-12)


TON_LO, TON_HI = 10, 128            # FFT bins used for tonality (about 310 to 4000 Hz)
MOD_RANGES = [(1, 2), (3, 4), (5, 8), (9, 16), (17, 30)]   # modulation bins (about 1 Hz each)
NFEAT = 4 * NMEL + 6 + 4 * len(MOD_RANGES)                 # 154


def features(x16):
    """x16: int16 array of WIN samples -> NFEAT features (level independent)."""
    x = x16.astype(np.float64) / 32768.0
    idx = np.arange(NFFT)[None, :] + HOP * np.arange(NFRAMES)[:, None]
    frames = x[idx] * HANN
    spec = np.fft.rfft(frames, axis=1)
    p = spec.real ** 2 + spec.imag ** 2
    mel = p @ FB.T                                   # NFRAMES x NMEL
    db = 10.0 * np.log10(mel + 1e-10)
    mean = db.mean(axis=0)
    rel = mean - mean.mean()                         # spectral shape, gain removed
    std = db.std(axis=0)                             # how much each band changes
    dif = np.abs(np.diff(db, axis=0)).mean(axis=0)   # how fast it changes (beeps, speech)
    peak = db.max(axis=0) - mean                     # short loud parts in each band
    # tonality: alarms are "peaky" (one strong pitch), noise is flat
    q = p[:, TON_LO:TON_HI + 1]
    am = q.mean(axis=1)
    ton = 10 * np.log10((q.max(axis=1) + 1e-12) / (am + 1e-12))
    gm = np.exp(np.log(q + 1e-12).mean(axis=1))
    flat = 10 * np.log10((gm + 1e-12) / (am + 1e-12))
    tonal = [ton.mean(), ton.std(), ton.max(), flat.mean(), flat.std(), flat.min()]
    # modulation: how the loudness of 4 pitch groups repeats (beeps 1-8 Hz, speech 3-8 Hz)
    mods = []
    t = np.arange(NFRAMES)
    for g in range(4):
        e = db[:, 8 * g:8 * g + 8].mean(axis=1)
        e = e - e.mean()
        X = np.array([np.abs(np.sum(e * np.exp(-2j * np.pi * k * t / NFRAMES))) ** 2 for k in range(1, 31)])
        tot = X.sum() + 1e-9
        for lo, hi in MOD_RANGES:
            mods.append(X[lo - 1:hi].sum() / tot)
    return np.concatenate([rel, std, dif, peak, tonal, mods]).astype(np.float32)


# ---------------- loading audio ----------------
def read_wav(path):
    with wave.open(path, "rb") as w:
        n, ch, sw, rate = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        raw = w.readframes(n)
    if sw != 2:
        return None
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    if rate != FS:
        g = np.gcd(FS, rate)
        x = resample_poly(x, FS // g, rate // g)
    return x


def to16(x):
    return np.clip(np.round(x), -32768, 32767).astype(np.int16)


def windows(x, hop=WIN // 2, drop_silent_db=20.0):
    """Cut 1 s windows. Drop windows much quieter than the loudest one (silent padding)."""
    if len(x) < WIN:
        x = np.concatenate([x, np.zeros(WIN - len(x))])
    out = [x[s:s + WIN] for s in range(0, len(x) - WIN + 1, hop)]
    lv = np.array([10 * np.log10(np.mean(w * w) + 1e-9) for w in out])
    keep = lv > lv.max() - drop_silent_db
    return [w for w, k in zip(out, keep) if k]


rng = np.random.default_rng(7)


def mic_augment(x):
    """Pretend the sound went through a different cheap mic: tilt, high-pass, gain, hiss."""
    a = rng.uniform(-0.6, 0.6)                       # spectral tilt
    y = lfilter([1, a], [1], x)
    fc = rng.uniform(60, 200)                        # coupling-capacitor high-pass
    r = np.exp(-2 * np.pi * fc / FS)
    y = lfilter([1, -1], [1, -r], y)
    y = y / (np.sqrt(np.mean(y * y)) + 1e-9) * 10 ** (rng.uniform(-40, -12) / 20) * 32768
    y = y + rng.standard_normal(len(y)) * 32768 * 10 ** (rng.uniform(-75, -55) / 20)
    return y


def mix(a, b, snr_db):
    pa, pb = np.mean(a * a) + 1e-9, np.mean(b * b) + 1e-9
    return a + b * np.sqrt(pa / pb / 10 ** (snr_db / 10))


def synth_alarm():
    """Beeping or sweeping tone like a reversing alarm or buzzer."""
    t = np.arange(WIN) / FS
    f = rng.uniform(500, 3000)
    kind = rng.integers(3)
    if kind == 0:   # on/off beeps
        rate = rng.uniform(1, 4)
        duty = rng.uniform(0.3, 0.7)
        gate = ((t * rate + rng.uniform()) % 1.0) < duty
        sig = np.sin(2 * np.pi * f * t) * gate
    elif kind == 1:  # two-tone
        rate = rng.uniform(1, 3)
        f2 = f * rng.uniform(1.15, 1.5)
        sel = ((t * rate) % 1.0) < 0.5
        sig = np.where(sel, np.sin(2 * np.pi * f * t), np.sin(2 * np.pi * f2 * t))
    else:            # sweep (siren)
        rate = rng.uniform(0.5, 3)
        inst = f * (1 + 0.4 * np.sin(2 * np.pi * rate * t))
        sig = np.sin(2 * np.pi * np.cumsum(inst) / FS)
    sig = sig + 0.3 * np.sign(sig) * rng.uniform(0, 1)   # buzzers are not pure tones
    return sig * 10000


def synth_machine():
    """Motor/transformer hum with harmonics plus rumble."""
    t = np.arange(WIN) / FS
    f0 = rng.uniform(40, 300)
    sig = sum(np.sin(2 * np.pi * f0 * k * t + rng.uniform(0, 6)) * rng.uniform(0.1, 1) / k for k in range(1, 8))
    noise = lfilter([1], [1, -rng.uniform(0.8, 0.99)], rng.standard_normal(WIN))
    sig = sig / np.std(sig) + rng.uniform(0.2, 1.5) * noise / np.std(noise)
    return sig * 8000


def speech_window(clips):
    """Join random spoken digits from one speaker into 1 s of talking."""
    out = np.zeros(0)
    while len(out) < WIN + FS // 4:
        c = clips[rng.integers(len(clips))]
        out = np.concatenate([out, c, np.zeros(rng.integers(FS // 40, FS // 6))])
    s = rng.integers(0, FS // 4)
    return out[s:s + WIN]


# ---------------- build the dataset ----------------
def build(args):
    data = {"train": [], "val": [], "test": []}   # lists of (int16 window, label)

    def add(split, x, label, augment=True, tag="synthetic", clip=-1):
        y = mic_augment(x) if augment else x
        data[split].append((to16(y), label, tag, clip))

    machine_pool = {"train": [], "val": [], "test": []}
    if args.esc50:
        rows = list(csv.DictReader(open(os.path.join(args.esc50, "meta", "esc50.csv"))))
        cat2lab = {}
        for lab, cats in ESC_MAP.items():
            for c in cats:
                cat2lab[c] = lab
        for r in rows:
            lab = cat2lab.get(r["category"], "other")
            if lab == "skip":
                continue
            split = "test" if r["fold"] == "5" else ("val" if r["fold"] == "4" else "train")
            x = read_wav(os.path.join(args.esc50, "audio", r["filename"]))
            if x is None:
                continue
            for w in windows(x):
                add(split, w, lab, tag="esc50:" + r["category"], clip=rows.index(r))
                if split == "train" and lab in ("alarm", "machine"):
                    add(split, w, lab)          # second augmented copy of the classes we care most about
                if lab == "machine":
                    machine_pool[split].append(w)
        print("ESC-50 loaded")

    if args.fsdd:
        spk = {}
        for f in glob.glob(os.path.join(args.fsdd, "recordings", "*.wav")):
            s = os.path.basename(f).split("_")[1]
            x = read_wav(f)
            if x is not None:
                spk.setdefault(s, []).append(x)
        names = sorted(spk)
        test_spk, val_spk = names[-1], names[-2]
        for s, clips in spk.items():
            split = "test" if s == test_spk else ("val" if s == val_spk else "train")
            n = {"train": 700, "val": 150, "test": 300}[split] // (1 if split != "train" else 4)
            for _ in range(n):
                add(split, speech_window(clips), "voice", tag="speech")
        print(f"FSDD loaded (test speaker {test_spk}, validation speaker {val_spk})")

    # synthetic alarms and machines, and mixtures (alarm over machine = alarm, talking over machine = voice)
    for split, n in (("train", 1200), ("val", 200), ("test", 300)):
        pool = machine_pool[split] or [synth_machine() for _ in range(50)]
        for _ in range(n // 4):
            add(split, synth_alarm(), "alarm", tag="beeps")
            add(split, synth_machine(), "machine")
            m = pool[rng.integers(len(pool))] if rng.uniform() < 0.6 else synth_machine()
            add(split, mix(synth_alarm(), m, rng.uniform(-6, 10)), "alarm", tag="beeps+machine")
        if args.esc50:
            alarms = [it[0] for it in data[split] if it[1] == "alarm"]
            for _ in range(n // 4):
                a = alarms[rng.integers(len(alarms))].astype(np.float64)
                m = pool[rng.integers(len(pool))]
                add(split, mix(a, m, rng.uniform(-3, 10)), "alarm", tag="esc50 alarm+machine")
        if args.fsdd:
            voices = [it[0] for it in data[split] if it[1] == "voice"]
            for _ in range(n // 4):
                v = voices[rng.integers(len(voices))].astype(np.float64)
                m = pool[rng.integers(len(pool))] if rng.uniform() < 0.6 else synth_machine()
                add(split, mix(v, m, rng.uniform(0, 12)), "voice", tag="speech+machine")

    if args.own:
        for lab in LABELS:
            files = sorted(glob.glob(os.path.join(args.own, lab, "*.wav")))
            for i, f in enumerate(files):
                split = "test" if i % 5 == 0 else ("val" if i % 5 == 1 else "train")
                x = read_wav(f)
                if x is None:
                    continue
                for w in windows(x, drop_silent_db=30):
                    add(split, w, lab, augment=False, tag="own", clip=10**6 + i)
                    if split == "train":
                        for _ in range(3):
                            add(split, w, lab)
        print("own recordings loaded")
    return data


def to_xy(items):
    X = np.stack([features(it[0]) for it in items])
    y = np.array([LABELS.index(it[1]) for it in items])
    return X, y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--esc50")
    ap.add_argument("--fsdd")
    ap.add_argument("--own")
    ap.add_argument("--out", default="anchor_ai_model.h")
    ap.add_argument("--report", default="anchor_ai_report.json")
    args = ap.parse_args()
    if not (args.esc50 or args.fsdd or args.own):
        sys.exit("Give at least one of --esc50, --fsdd, --own")

    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.metrics import confusion_matrix, accuracy_score

    data = build(args)
    Xtr, ytr = to_xy(data["train"])
    Xva, yva = to_xy(data["val"])
    Xte, yte = to_xy(data["test"])
    print("windows: train", len(ytr), "val", len(yva), "test", len(yte))
    print("train per class:", {LABELS[i]: int((ytr == i).sum()) for i in range(len(LABELS))})

    # balance classes by repeating the smaller ones
    counts = np.bincount(ytr, minlength=len(LABELS))
    want = {c: counts.max() * (2 if LABELS[c] == "alarm" else 1) for c in range(len(LABELS))}
    idx = np.concatenate([rng.choice(np.where(ytr == c)[0], want[c], replace=True) for c in range(len(LABELS))])
    Xb, yb = Xtr[idx], ytr[idx]
    best, best_acc = None, -1
    for leaves in (15, 31):
        clf = HistGradientBoostingClassifier(max_iter=300, learning_rate=0.08, max_leaf_nodes=leaves,
                                             early_stopping=True, random_state=0)
        clf.fit(Xb, yb)
        acc = accuracy_score(yva, clf.predict(Xva))
        ia_ = LABELS.index("alarm")
        rec = float(np.mean(clf.predict_proba(Xva)[yva == ia_, ia_] >= 0.4))
        print(f"  trees with {leaves} leaves: validation accuracy {acc:.3f}, alarms caught {rec:.3f} ({clf.n_iter_} rounds)")
        score = acc + rec    # choose on accuracy AND alarm safety
        if score > best_acc:
            best, best_acc, best_val = clf, score, acc

    pred = best.predict(Xte)
    prob = best.predict_proba(Xte)
    cm = confusion_matrix(yte, pred, labels=range(len(LABELS)))
    acc = accuracy_score(yte, pred)
    ia = LABELS.index("alarm")
    alarm_recall = cm[ia, ia] / max(cm[ia].sum(), 1)
    alarm_04 = float(np.mean(prob[yte == ia, ia] >= 0.4))   # what sketch 09 uses (ALARM_TRIGGER 0.40)
    false_alarm_04 = float(np.mean(prob[yte != ia, ia] >= 0.4))
    print(f"\nTEST accuracy {acc:.3f}   alarm recall {alarm_recall:.3f}   alarms caught at 0.40: {alarm_04:.3f}   "
          f"non-alarms flagged as alarm at 0.40: {false_alarm_04:.3f}")
    print("confusion matrix (rows = true, columns = predicted):", LABELS)
    print(cm)
    # per source: how often the alarm score reaches 0.40
    tags = [it[2] for it in data["test"]]
    per = {}
    for i, tg in enumerate(tags):
        per.setdefault((LABELS[yte[i]], tg if yte[i] == ia else LABELS[yte[i]] + " (" + tg.split(":")[0] + ")"), []).append(prob[i, ia] >= 0.4)
    print("alarm score >= 0.40, by source (true label, source): share")
    for k in sorted(per):
        print(f"   {k}: {np.mean(per[k]):.2f}  (n={len(per[k])})")
    # clip level: a real alarm lasts seconds. Is it caught within the first 3 windows (about 1.5 s)?
    clips = {}
    for i, it in enumerate(data["test"]):
        if it[3] >= 0:
            clips.setdefault(it[3], []).append((yte[i], prob[i, ia]))
    a_hit = [max(p for _, p in v[:3]) >= 0.4 for v in clips.values() if v[0][0] == ia]
    m_false = [max(p for _, p in v) >= 0.4 for v in clips.values() if v[0][0] == LABELS.index("machine")]
    o_false = [max(p for _, p in v) >= 0.4 for v in clips.values() if v[0][0] == LABELS.index("other")]
    clip_alarm = float(np.mean(a_hit)) if a_hit else None
    clip_mfalse = float(np.mean(m_false)) if m_false else None
    print(f"CLIPS: alarm clips caught within ~1.5 s: {clip_alarm}  (n={len(a_hit)});  machine clips with any false ALARM in 5 s: {clip_mfalse} (n={len(m_false)});  other clips with a false ALARM: {np.mean(o_false) if o_false else None}")

    json.dump({"labels": LABELS, "test_accuracy": acc, "alarm_recall": alarm_recall,
               "alarm_caught_at_0.40": alarm_04, "false_alarm_at_0.40": false_alarm_04,
               "confusion": cm.tolist(), "n_train": int(len(ytr)), "n_test": int(len(yte)),
               "val_accuracy": best_val, "clip_alarm_caught_1_5s": clip_alarm,
               "clip_machine_false_alarm_5s": clip_mfalse}, open(args.report, "w"), indent=1)
    export_c(best, args.out)
    import pickle
    pickle.dump(best, open(os.path.splitext(args.out)[0] + ".pkl", "wb"))
    check_export(best, Xte)
    np.savez(os.path.splitext(args.out)[0] + "_testset.npz", X=Xte, y=yte, prob=prob)
    print("wrote", args.out)


def carr(name, a):
    flat = ", ".join(f"{v:.8g}f" for v in np.asarray(a, dtype=np.float64).ravel())
    return f"static const float {name}[{np.asarray(a).size}] = {{ {flat} }};\n"


def tree_list(clf):
    """Flatten the boosted trees: one tree per (round, class)."""
    out = []
    for rnd in clf._predictors:
        for c, tp in enumerate(rnd):
            out.append((c, tp.nodes))
    return out


def export_c(clf, path):
    trees = tree_list(clf)
    feat, thr, left, right, start, cls = [], [], [], [], [], []
    for c, nodes in trees:
        start.append(len(feat)); cls.append(c)
        for nd in nodes:
            if nd["is_leaf"]:
                feat.append(-1); thr.append(float(nd["value"])); left.append(0); right.append(0)
            else:
                feat.append(int(nd["feature_idx"])); thr.append(float(nd["num_threshold"]))
                left.append(int(nd["left"])); right.append(int(nd["right"]))
    base = np.asarray(clf._baseline_prediction, dtype=np.float64).ravel()
    s = "// Generated by train_anchor_ai.py - do not edit by hand.\n#pragma once\n#include <stdint.h>\n"
    s += f"#define AAI_NMEL {NMEL}\n#define AAI_NFEAT {NFEAT}\n#define AAI_NCLASS {len(LABELS)}\n"
    s += f"#define AAI_NTREES {len(trees)}\n#define AAI_NNODES {len(feat)}\n"
    starts, counts, weights = [], [], []
    for m in range(NMEL):
        nz = np.nonzero(FB[m])[0]
        starts.append(int(nz[0])); counts.append(int(len(nz))); weights.extend(FB[m, nz[0]:nz[-1] + 1])
    s += "static const uint16_t AAI_MEL_START[AAI_NMEL] = { " + ", ".join(map(str, starts)) + " };\n"
    s += "static const uint16_t AAI_MEL_COUNT[AAI_NMEL] = { " + ", ".join(map(str, counts)) + " };\n"
    s += carr("AAI_MEL_W", weights)
    s += carr("AAI_BASE", base)
    s += "static const uint32_t AAI_TREE_START[AAI_NTREES] = { " + ", ".join(map(str, start)) + " };\n"
    s += "static const uint8_t AAI_TREE_CLASS[AAI_NTREES] = { " + ", ".join(map(str, cls)) + " };\n"
    s += "static const int16_t AAI_NODE_FEAT[AAI_NNODES] = { " + ", ".join(map(str, feat)) + " };\n"
    s += carr("AAI_NODE_THR", thr)
    s += "static const uint16_t AAI_NODE_LEFT[AAI_NNODES] = { " + ", ".join(map(str, left)) + " };\n"
    s += "static const uint16_t AAI_NODE_RIGHT[AAI_NNODES] = { " + ", ".join(map(str, right)) + " };\n"
    open(path, "w").write(s)
    print(f"exported {len(trees)} trees, {len(feat)} nodes")


def trees_predict(clf, X):
    """Re-implementation of what the C code does, to check the export."""
    trees = tree_list(clf)
    raw = np.tile(np.asarray(clf._baseline_prediction, dtype=np.float64).ravel(), (len(X), 1))
    Xf = X.astype(np.float32)
    for c, nodes in trees:
        for i in range(len(X)):
            n = 0
            while not nodes[n]["is_leaf"]:
                n = nodes[n]["left"] if Xf[i, nodes[n]["feature_idx"]] <= np.float32(nodes[n]["num_threshold"]) else nodes[n]["right"]
            raw[i, c] += nodes[n]["value"]
    e = np.exp(raw - raw.max(axis=1, keepdims=True))
    return e / e.sum(axis=1, keepdims=True)


def check_export(clf, X):
    sub = X[:300]
    diff = np.abs(trees_predict(clf, sub) - clf.predict_proba(sub)).max()
    print(f"export check: largest difference to scikit-learn = {diff:.2e}")


if __name__ == "__main__":
    main()

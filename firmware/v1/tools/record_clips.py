"""
ANCHOR - record labelled sound clips from the headset's REF mic.

Install once:   pip install pyserial
Run, e.g.:      python record_clips.py --port COM5 --label machine --count 10
                (on Mac/Linux the port looks like /dev/ttyUSB0 or /dev/cu.usbserial-xxxx)

Each clip is saved as  dataset/<label>/<label>.<date_time>.wav  (16 kHz, mono, 16-bit).
train_anchor_ai.py reads the label from the folder name (dataset/machine, dataset/alarm, ...).
"""
import argparse, os, struct, sys, time, wave
from datetime import datetime

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Run:  pip install pyserial")

RATE = 16000


def read_exact(ser, n, timeout_s):
    data = bytearray()
    t0 = time.time()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if chunk:
            data += chunk
            t0 = time.time()
        elif time.time() - t0 > timeout_s:
            raise TimeoutError("board stopped sending")
    return bytes(data)


def record(ser, seconds):
    ser.reset_input_buffer()
    ser.write(f"R{seconds}\n".encode())
    window = b""
    t0 = time.time()
    while not window.endswith(b"AUD0"):
        b = ser.read(1)
        if b:
            window = (window + b)[-4:]
        if time.time() - t0 > 5:
            raise TimeoutError("no answer from the board - is sketch 06 uploaded and the Serial Monitor closed?")
    (count,) = struct.unpack("<I", read_exact(ser, 4, 3))
    raw = read_exact(ser, count * 2, 3)
    end = read_exact(ser, 4, 3)
    if end != b"END0":
        print("  warning: clip ending looked wrong, check this clip by listening to it")
    return raw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="e.g. COM5 or /dev/ttyUSB0")
    ap.add_argument("--label", required=True, help="machine, alarm, voice or other")
    ap.add_argument("--seconds", type=int, default=10)
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--out", default="dataset")
    ap.add_argument("--baud", type=int, default=921600)
    a = ap.parse_args()

    folder = os.path.join(a.out, a.label)
    os.makedirs(folder, exist_ok=True)
    ser = serial.Serial(a.port, a.baud, timeout=0.5)
    time.sleep(2.0)   # the board may restart when the port opens
    print(f"Recording {a.count} clips of '{a.label}', {a.seconds} s each.")
    for i in range(a.count):
        input(f"\nClip {i + 1}/{a.count}: get the sound ready, then press Enter...")
        print("  recording...", flush=True)
        raw = record(ser, a.seconds)
        samples = struct.unpack(f"<{len(raw) // 2}h", raw)
        peak = max(abs(s) for s in samples) if samples else 0
        name = f"{a.label}.{datetime.now().strftime('%Y%m%d_%H%M%S')}_{i + 1:02d}.wav"
        path = os.path.join(folder, name)
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(raw)
        note = ""
        if peak >= 32000:
            note = "  <-- TOO LOUD (clipping). Lower IN_VOLUME in sketch 06 or move away."
        elif peak < 300 and a.label != "quiet":
            note = "  <-- very quiet. Is the REF mic working? Raise IN_VOLUME a little."
        print(f"  saved {path}  (peak {peak}){note}")
    ser.close()
    print("\nDone. Listen to a few clips on your laptop to check them.")


if __name__ == "__main__":
    main()

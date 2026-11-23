# =====================================================================
# ANCHOR - play a song from your laptop through the headset speaker
#
# 1. Upload sketch 10_music_player to the ESP32 and CLOSE the Serial Monitor.
# 2. Install once:   pip install pyserial numpy
# 3. Run:            python play_music.py --port COM17 --file song.wav
#
# The file must be a normal 16-bit WAV (any sample rate, mono or stereo).
# If you have an MP3, convert it to WAV first (for example with Audacity or
# VLC: Media > Convert/Save), or record from a phone and export as WAV.
#
# Options:
#   --volume 20    speaker volume 0..60 (the board never goes above 60)
#   --start 30     start 30 seconds into the song
#   --seconds 60   play only 60 seconds
#   --baud 921600  must match BAUD in the sketch
# Stop with Ctrl+C.
#
# The sound is converted to 16 kHz mono (notes up to 8 kHz), like a radio.
# =====================================================================
import argparse, sys, time, wave
import numpy as np

FS = 16000
CHUNK = 1024          # must match CHUNK in the sketch


def load_wav(path):
    try:
        w = wave.open(path, "rb")
    except Exception as e:
        sys.exit("Could not open %s as a WAV file: %s\n(Convert MP3 to WAV first.)" % (path, e))
    ch, sw, fs, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 1:
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) / 128.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float64) / (1 << 23)
    elif sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        sys.exit("Unsupported WAV format (%d bytes per sample)." % sw)
    x = x[: (len(x) // ch) * ch].reshape(-1, ch).mean(axis=1)   # to mono
    return x, fs


def to_16k(x, fs):
    if fs == FS:
        return x
    if fs > FS:
        # low-pass below 7 kHz first, so high notes do not fold back as whistles
        taps = 127
        fc = 7000.0 / fs
        n = np.arange(taps) - (taps - 1) / 2
        h = np.sinc(2 * fc * n) * np.hanning(taps)
        h /= h.sum()
        x = np.convolve(x, h, mode="same")
    t = np.arange(0, len(x) - 1, fs / FS)
    return np.interp(t, np.arange(len(x)), x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="for example COM17")
    ap.add_argument("--file", required=True, help="16-bit WAV file")
    ap.add_argument("--volume", type=int, default=20, help="0..60 (default 20)")
    ap.add_argument("--start", type=float, default=0, help="start at this second")
    ap.add_argument("--seconds", type=float, default=0, help="play only this many seconds (0 = all)")
    ap.add_argument("--baud", type=int, default=921600)
    a = ap.parse_args()

    try:
        import serial
    except Exception:
        sys.exit("Install pyserial first:  pip install pyserial")

    vol = max(0, min(60, a.volume))
    x, fs = load_wav(a.file)
    x = to_16k(x, fs)
    s0 = int(a.start * FS)
    x = x[s0:]
    if a.seconds > 0:
        x = x[: int(a.seconds * FS)]
    if len(x) < FS // 2:
        sys.exit("Nothing to play (the file is too short or --start is past the end).")
    peak = float(np.max(np.abs(x))) + 1e-9
    x = x * (0.8 / peak)                    # use the same headroom for every song
    pcm = np.clip(x * 32767.0, -32768, 32767).astype("<i2")
    pad = (-len(pcm)) % CHUNK
    if pad:
        pcm = np.concatenate([pcm, np.zeros(pad, dtype="<i2")])
    total_chunks = len(pcm) // CHUNK
    print("Playing %s: %.1f s, volume %d (max 60). Ctrl+C to stop." % (a.file, len(pcm) / FS, vol))

    try:
        ser = serial.Serial(a.port, a.baud, timeout=2)
    except Exception as e:
        sys.exit("Could not open %s: %s\nClose the Arduino Serial Monitor and check the port number." % (a.port, e))

    try:
        time.sleep(3.0)                     # the board may restart when the port opens
        ser.reset_input_buffer()
        ser.write(b"P" + bytes([vol]))
        sent = 0
        t0 = time.time()
        last = 0
        while sent < total_chunks:
            c = ser.read(1)
            if c == b"":
                sys.exit("The board did not answer. Is sketch 10_music_player uploaded and the Serial Monitor closed?\n"
                         "Press the RESET button on the ESP32 and try again.")
            if c != b"N":
                continue
            ser.write(b"D" + pcm[sent * CHUNK:(sent + 1) * CHUNK].tobytes())
            sent += 1
            if time.time() - last > 1.0:
                last = time.time()
                print("\r  %5.1f / %.1f s" % (sent * CHUNK / FS, len(pcm) / FS), end="", flush=True)
        # tell the board the song is over, then let the last second play
        c = ser.read(1)
        while c not in (b"N", b""):
            c = ser.read(1)
        ser.write(b"Q")
        time.sleep(1.5)
        print("\nDone.")
    except KeyboardInterrupt:
        try:
            ser.write(b"Q")
        except Exception:
            pass
        print("\nStopped.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()

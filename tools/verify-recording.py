#!/usr/bin/env python3
"""Generate a test signal, and verify what the engine recorded from it.

This is the part that decides whether a recording is trustworthy, so it checks
the properties that matter and says which one failed. A test that only asserts
"a file exists" would have passed every defect this project has had.
"""

import argparse
import math
import os
import re
import struct
import sys
import wave


def tone_hz(track_index):
    """One distinct frequency per track, all safely below Nyquist.

    The first version of this was 220 * 1.5**n, which reaches 216 kHz by
    track 18 -- far above Nyquist at 48 kHz, so those tracks recorded aliased
    tones. The RMS check passed anyway, because a folded sine has the same
    amplitude as the one that produced it. Only the frequency check below
    exposed it.

    Linear spacing keeps track 18 at 5.6 kHz. The offsets are not integer
    ratios of each other, so even a nonlinearity somewhere in the path cannot
    make one track's harmonic look like another track's fundamental.
    """
    return 250.0 + 317.0 * track_index


def generate(path, tracks, rate, seconds):
    nyquist = rate / 2.0
    highest = tone_hz(tracks - 1)
    if highest >= nyquist * 0.9:
        raise SystemExit(
            f"test signal would alias: track {tracks} tone is {highest:.0f} Hz"
            f" against a Nyquist of {nyquist:.0f} Hz")

    frames = int(rate * seconds)
    with wave.open(path, "wb") as w:
        w.setnchannels(tracks)
        w.setsampwidth(2)
        w.setframerate(rate)
        buf = bytearray()
        for n in range(frames):
            for ch in range(tracks):
                # 0.5 amplitude leaves headroom, so a clipped result means the
                # engine introduced gain rather than the source being hot.
                v = 0.5 * math.sin(2.0 * math.pi * tone_hz(ch) * n / rate)
                buf += struct.pack("<h", int(v * 32767))
        w.writeframes(bytes(buf))
    print(f"generated {path}: {tracks}ch {rate}Hz {seconds}s")


def read_bext_time_reference(path):
    """Returns the BWF TimeReference, or None if there is no bext chunk."""
    with open(path, "rb") as f:
        if f.read(4) != b"RIFF":
            return None
        f.read(4)
        if f.read(4) != b"WAVE":
            return None
        while True:
            header = f.read(8)
            if len(header) < 8:
                return None
            tag, size = struct.unpack("<4sI", header)
            payload = f.read(size + (size & 1))
            if tag == b"bext":
                low, high = struct.unpack("<II", payload[338:346])
                return (high << 32) | low


def goertzel_power(samples, rate, freq):
    """Energy at one frequency, via the Goertzel algorithm.

    Cheaper and more direct than an FFT when the question is "is *this* tone
    present", which is exactly the question here.
    """
    n = len(samples)
    if n == 0:
        return 0.0
    k = int(0.5 + (n * freq) / rate)
    omega = (2.0 * math.pi * k) / n
    coeff = 2.0 * math.cos(omega)
    s_prev = 0.0
    s_prev2 = 0.0
    for x in samples:
        s = x + coeff * s_prev - s_prev2
        s_prev2 = s_prev
        s_prev = s
    return s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2


def dominant_track(samples, rate, tracks):
    """Which track's tone is loudest in these samples.

    This is what catches a channel written to the wrong file. Checking only
    that each file has signal would pass on any permutation of the inputs,
    and a permutation is precisely what a lexicographic port sort produces on
    an 18-channel interface.
    """
    best_index = -1
    best_power = 0.0
    for i in range(tracks):
        p = goertzel_power(samples, rate, tone_hz(i))
        if p > best_power:
            best_power = p
            best_index = i
    return best_index, best_power


def rms_dbfs(samples):
    if not samples:
        return -999.0
    total = sum(s * s for s in samples)
    r = math.sqrt(total / len(samples))
    return 20.0 * math.log10(r) if r > 0 else -999.0


def read_int24_mono(path, limit_frames):
    with open(path, "rb") as f:
        data = f.read()
    idx = data.find(b"data")
    if idx < 0:
        return None
    size = struct.unpack("<I", data[idx + 4:idx + 8])[0]
    payload = data[idx + 8:idx + 8 + size]
    out = []
    count = min(len(payload) // 3, limit_frames)
    for i in range(count):
        b0, b1, b2 = payload[i * 3], payload[i * 3 + 1], payload[i * 3 + 2]
        v = b0 | (b1 << 8) | (b2 << 16)
        if v & 0x800000:
            v -= 0x1000000
        out.append(v / 8388608.0)
    return out


def track_index_from_name(name):
    """"Audio 7_01.wav" -> 6. Returns None if the name is not ours."""
    m = re.match(r"Audio (\d+)_\d+\.wav$", name)
    if not m:
        return None
    return int(m.group(1)) - 1


def check(directory, tracks, rate, seconds):
    failures = []
    expected_frames = int(rate * seconds)

    if not os.path.isdir(directory):
        print(f"FAIL: {directory} does not exist", file=sys.stderr)
        return 1

    files = sorted(f for f in os.listdir(directory) if f.endswith(".wav"))
    print(f"found {len(files)} file(s) in {directory}")
    if len(files) != tracks:
        failures.append(f"expected {tracks} files, found {len(files)}")

    for name in files:
        path = os.path.join(directory, name)
        size = os.path.getsize(path)

        # --- the header has to be readable by a stock WAV parser ---
        try:
            with wave.open(path, "rb") as w:
                channels = w.getnchannels()
                width = w.getsampwidth()
                got_rate = w.getframerate()
                frames = w.getnframes()
        except Exception as exc:
            failures.append(f"{name}: not a readable WAV ({exc})")
            continue

        print(f"  {name}: {size}B {channels}ch {width * 8}bit "
              f"{got_rate}Hz {frames} frames")

        if got_rate != rate:
            failures.append(f"{name}: rate {got_rate}, expected {rate}")
        if width != 3:
            failures.append(f"{name}: {width * 8}-bit, expected 24-bit")

        # A short file means the disk thread lost data. Allow one block of
        # slack at the tail, not an arbitrary fraction.
        if frames < expected_frames - 1024:
            failures.append(
                f"{name}: {frames} frames, expected about {expected_frames}")

        # --- the timecode origin is the whole point of writing BWF ---
        tr = read_bext_time_reference(path)
        if tr is None:
            failures.append(f"{name}: no bext chunk, so no timecode origin")

        # --- there has to be actual signal in it ---
        samples = read_int24_mono(path, min(frames, rate))
        if samples is None:
            failures.append(f"{name}: no data chunk")
            continue
        level = rms_dbfs(samples)
        print(f"    level: {level:.1f} dBFS")
        if level < -60.0:
            failures.append(f"{name}: silent ({level:.1f} dBFS)")
        if level > -1.0:
            failures.append(
                f"{name}: {level:.1f} dBFS is hotter than the source, so the"
                " engine added gain")

        # --- is this the channel it is supposed to be? ---
        # Track N must carry tone N. Without this the whole suite passes on
        # any permutation of the inputs, which is what a lexicographic port
        # sort produces, and the reason the file count was raised to 18.
        expected = track_index_from_name(name)
        if expected is None:
            failures.append(f"{name}: cannot tell which track this is")
        elif level >= -60.0:
            got, _ = dominant_track(samples, rate, tracks)
            if got != expected:
                failures.append(
                    f"{name}: carries tone {got} ({tone_hz(got):.0f} Hz) but"
                    f" should carry tone {expected}"
                    f" ({tone_hz(expected):.0f} Hz) -- channels are crossed")
            else:
                print(f"    tone:  {tone_hz(expected):.0f} Hz, correct track")

    if failures:
        print()
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1

    print("\nPASS: every file is well-formed, full length, and carries signal.")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generate", metavar="PATH")
    ap.add_argument("--check", metavar="DIR")
    ap.add_argument("--tracks", type=int, default=4)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--seconds", type=float, default=3.0)
    args = ap.parse_args()

    if args.generate:
        generate(args.generate, args.tracks, args.rate, args.seconds)
        return 0
    if args.check:
        return check(args.check, args.tracks, args.rate, args.seconds)
    ap.error("pass --generate or --check")


if __name__ == "__main__":
    sys.exit(main())

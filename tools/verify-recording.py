#!/usr/bin/env python3
"""Generate a test signal, and verify what the engine recorded from it.

This is the part that decides whether a recording is trustworthy, so it checks
the properties that matter and says which one failed. A test that only asserts
"a file exists" would have passed every defect this project has had.
"""

import argparse
import math
import os
import struct
import sys
import wave


def tone_hz(track_index):
    # Distinct, non-harmonic frequencies so a channel written to the wrong file
    # is detectable rather than merely suspicious.
    return 220.0 * (1.5 ** track_index)


def generate(path, tracks, rate, seconds):
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

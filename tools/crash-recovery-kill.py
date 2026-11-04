#!/usr/bin/env python3
"""A session recovered after the engine dies mid-take, with no hardware.

The software half of system.hardening.crash_recovery. A power pull cannot be
run here; SIGKILL is its closest process-level stand-in: the engine gets no
chance to close a file, flush a buffer or write a header. (A SIGKILL does not
discard what the kernel already holds, which a power cut does; that part,
and the stick's own write cache, stays with the hardware test.)

The real engine runs with --backend fake and records on every track. Once the
take files hold some seconds of audio it is killed with SIGKILL, then a second
engine opens the same folder, as the unit's restart would. This checks that

  - the second engine starts and says it recovered each take;
  - every take is a finished WAV whose RIFF and data sizes count exactly what
    the file holds (the header is repaired, nothing is cut off the end);
  - nothing the first engine had put on disk before the kill is lost: the
    repaired take holds at least as many bytes as the file had when the kill
    was sent;
  - the session folder opens: the restarted engine listens and leaves cleanly.

Usage:  tools/crash-recovery-kill.py --engine build/sonora-engine
Linux only.
"""

import argparse
import glob
import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def load_check_take():
    # The same finished-WAV verdict the failed-take test uses, one definition.
    spec = importlib.util.spec_from_file_location(
        "failed_take_quit", os.path.join(HERE, "failed-take-quit.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def takes(folder):
    return sorted(glob.glob(os.path.join(folder, "Audio Files", "*.wav")))


def run(engine, tracks, min_bytes, wait_seconds, logs):
    failures = []
    ftq = load_check_take()
    work = tempfile.mkdtemp(prefix="sonora-crash-")
    folder = os.path.join(work, "session")
    common = ["--dir", folder, "--tracks", str(tracks), "--backend", "fake",
              "--no-memlock", "--plugin-cache", "", "--autosave-seconds", "0"]
    log1 = open(os.path.join(logs, "crash-recovery-first.log"), "wb")
    first = subprocess.Popen([engine] + common + ["--arm-all", "--record", "600"],
                             stdin=subprocess.DEVNULL, stdout=log1, stderr=log1)
    try:
        deadline = time.monotonic() + wait_seconds
        sizes = {}
        while time.monotonic() < deadline:
            if first.poll() is not None:
                failures.append(f"the first engine exited {first.returncode} before the kill")
                return failures
            sizes = {t: os.path.getsize(t) for t in takes(folder)}
            if len(sizes) >= tracks and min(sizes.values()) >= min_bytes:
                break
            time.sleep(0.1)
        else:
            failures.append(f"no take reached {min_bytes} bytes on {tracks} tracks "
                            f"in {wait_seconds} s: {sizes}")
            return failures
        # Sizes read before the kill: the file only grows, so these are a floor.
        first.send_signal(signal.SIGKILL)
        first.wait(timeout=10)
        floor = {t: os.path.getsize(t) for t in takes(folder)}
        sizes = {t: max(sizes.get(t, 0), 0) for t in floor}
        print(f"killed with SIGKILL; {len(floor)} take files, "
              f"{min(floor.values())}..{max(floor.values())} bytes")
    finally:
        if first.poll() is None:
            first.kill()
            first.wait()
        log1.close()

    second_log = os.path.join(logs, "crash-recovery-second.log")
    with open(second_log, "wb") as out:
        second = subprocess.Popen([engine] + common + ["--headless"],
                                  stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT)
        listening = False
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and second.poll() is None:
            out.flush()
            with open(second_log, "rb") as f:
                if b"engine listening on" in f.read():
                    listening = True
                    break
            time.sleep(0.1)
        code = second.poll()
        if code is None:
            second.send_signal(signal.SIGTERM)
            try:
                code = second.wait(timeout=20)
            except subprocess.TimeoutExpired:
                second.kill()
                second.wait()
                failures.append("the restarted engine ignored SIGTERM for 20 s")
                code = None
    with open(second_log, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    if not listening:
        failures.append("the restarted engine never reached 'engine listening on'")
    elif code not in (0, None):
        failures.append(f"the restarted engine exited {code}, not 0")
    for t in floor:
        name = os.path.basename(t)
        if f"recovered {name}" not in text:
            failures.append(f"the restart did not report recovering {name}")

    for t, before in floor.items():
        name = os.path.basename(t)
        known, why = ftq.check_take(t, max_short=0)
        if why:
            failures.append(f"{name}: {why}")
            continue
        found, blob = ftq.chunks(t)
        data_at, data = found[b"data"]
        if data_at + data < before - 1:
            failures.append(f"{name}: recovered audio ends at byte {data_at + data}, "
                            f"but the file held {before} when the kill was sent")
    shutil.rmtree(work, ignore_errors=True)
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--engine", required=True)
    p.add_argument("--tracks", type=int, default=4)
    p.add_argument("--min-bytes", type=int, default=512 * 1024,
                   help="kill once every take file is this large")
    p.add_argument("--wait", type=int, default=60)
    p.add_argument("--logs", default=tempfile.gettempdir())
    args = p.parse_args()
    failures = run(os.path.abspath(args.engine), args.tracks, args.min_bytes,
                   args.wait, args.logs)
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if not failures:
        print("ok: killed mid-take, the session reopened with every take repaired "
              "(power pull on hardware: UNVERIFIED)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

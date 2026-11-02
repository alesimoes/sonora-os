#!/usr/bin/env python3
"""A quit during a failed take: exit 1, the terminal given back, the take kept.

The engine runs on a pseudo-terminal as it runs on tty1, with --backend fake
so no audio server or interface is needed. The take is made to fail the way
a full disk fails it: the engine starts under RLIMIT_FSIZE, with SIGXFSZ
ignored, so its writes past the limit return EFBIG. Once the interface shows
the failure, the session is quit, in each of the ways it is quit on the
appliance:

  key      the operator presses q
  engine   SIGTERM to the engine alone (kill, or a watchdog)
  session  SIGTERM to every process on the terminal, as systemd stops
           sonora-session.service (KillMode=control-group)

and for each one this checks that

  - the engine exits 1 (main.cpp: a quit in a failed take is no clean exit),
    within the time systemd allows before SIGKILL;
  - the terminal's modes read back exactly as they were before the engine
    started: the interface's raw mode does not outlive it;
  - every take file is a finished WAV whose header counts the audio it holds
    and nothing past it, which is what WavWriter::close() makes of a failed
    file;
  - the folder opens again: a second engine, headless, starts in it and
    leaves cleanly, as the unit's restart would.

Session::take_failure() is unit-tested; this is the half no unit test sees.
Linux only. Usage:

  tools/failed-take-quit.py --engine build/sonora-engine [--mode key ...]
"""

import argparse
import fcntl
import os
import re
import resource
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

# systemd's default TimeoutStopSec. Past it the unit is SIGKILLed, which
# leaves the terminal as the interface had it.
STOP_SECONDS = 90
FAILURE_RE = rb"take 1 failed: [^\r\n]*File too large"


class Screen:
    """What the engine has drawn, read from the pty master as it arrives.

    The parent keeps the slave open to read the modes back, so the master
    never sees EOF; reads are polled instead."""

    def __init__(self, fd, log):
        self.fd = fd
        self.data = bytearray()
        self.log = log

    def pump(self, seconds):
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return
            ready, _, _ = select.select([self.fd], [], [], min(left, 0.05))
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                return
            self.data.extend(chunk)
            self.log.write(chunk)

    def wait_for(self, pattern, since, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if re.search(pattern, bytes(self.data[since:])):
                return True
            self.pump(0.05)
        return False

    def mark(self):
        return len(self.data)


def modes(fd):
    # The speeds and the control characters too: a restore that put back
    # only the flags would still leave VMIN at 0.
    attrs = termios.tcgetattr(fd)
    return attrs[:6] + list(attrs[6])


def describe_modes(before, after):
    names = ["iflag", "oflag", "cflag", "lflag", "ispeed", "ospeed"]
    out = []
    for i, name in enumerate(names):
        if before[i] != after[i]:
            out.append(f"{name} {before[i]:#o} -> {after[i]:#o}")
    for i in range(6, len(before)):
        if before[i] != after[i]:
            out.append(f"c_cc[{i - 6}] {before[i]!r} -> {after[i]!r}")
    return ", ".join(out)


def session_pids(sid):
    """Every process whose session is sid: what the unit's cgroup holds."""
    pids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/stat", "rb") as f:
                stat = f.read()
        except OSError:
            continue
        # The command name is in parentheses and may hold spaces.
        fields = stat[stat.rindex(b")") + 2:].split()
        if int(fields[3]) == sid:
            pids.append(int(entry))
    return pids


def start_engine(engine, argv, slave, fsize):
    pid = os.fork()
    if pid != 0:
        return pid
    try:
        os.setsid()
        try:
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        except OSError:
            # Still another session's terminal; the headless restart needs
            # none.
            pass
        for fd in (0, 1, 2):
            os.dup2(slave, fd)
        if fsize > 0:
            # Ignored, not handled: an ignored disposition survives exec, and
            # with it a write past the limit fails with EFBIG instead of
            # killing the engine.
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (fsize, fsize))
        os.execv(engine, [engine] + argv)
    finally:
        os._exit(127)


def wait_exit(pid, screen, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            return os.waitstatus_to_exitcode(status)
        screen.pump(0.1)
    return None


def chunks(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        return None, blob
    found = {}
    pos = 12
    while pos + 8 <= len(blob):
        tag = blob[pos:pos + 4]
        size = struct.unpack_from("<I", blob, pos + 4)[0]
        found[tag] = (pos + 8, size)
        pos += 8 + size + (size % 2)
    return found, blob


# Known defect, not a regression: WavWriter::write() counts the frames stdio
# accepted, and a take that fails on a full disk fails at a later flush, so
# the frames still in the stdio buffer are counted and never written. The
# header then claims up to one buffer more than the file holds; close()
# cannot grow the file to match on a disk that is full, and the next open
# reports the take shorter than its header and plays what is there. Whether
# a run hits it depends on where the limit falls in the buffer, so it is
# reported as "xfail" and never fails the run; remove it with the fix.
KNOWN_SHORT = "header counts audio the file does not hold (WavWriter counts buffered frames)"


def check_take(path):
    """(None, None) for a finished WAV; else (known, what is wrong)."""
    found, blob = chunks(path)
    if found is None:
        return False, "not RIFF/WAVE"
    if b"fmt " not in found or b"data" not in found:
        return False, "no fmt or data chunk"
    riff = struct.unpack_from("<I", blob, 4)[0]
    fmt_at, _ = found[b"fmt "]
    block_align = struct.unpack_from("<H", blob, fmt_at + 12)[0]
    data_at, data = found[b"data"]
    if data == 0:
        return False, "the data chunk counts no audio"
    if data % block_align:
        return False, f"data size {data} is not whole frames of {block_align} bytes"
    data_end = data_at + data + (data % 2)
    if riff + 8 != data_end:
        return False, f"RIFF size {riff} + 8 disagrees with the data chunk's end at {data_end}"
    if data_end < len(blob):
        return False, (f"{len(blob) - data_end} bytes past the counted audio, "
                       "which close() should have cut")
    if data_end > len(blob):
        short = (data_end - len(blob)) // block_align
        return True, f"{KNOWN_SHORT}: {short} frames"
    return None, None


def kill_all(pids):
    for p in pids:
        try:
            os.kill(p, signal.SIGKILL)
        except OSError:
            pass


def run(args, mode):
    print(f"--- quit by {mode} ---")
    log_path = os.path.join(args.logs, f"failed-take-quit-{mode}.log")
    failures = run_one(args, mode, log_path)
    for f in failures:
        print(f"FAIL ({mode}): {f}", file=sys.stderr)
    if failures:
        print(f"screen: {log_path}", file=sys.stderr)
    return failures


def run_one(args, mode, log_path):
    work = tempfile.mkdtemp(prefix="sonora-quit-")
    folder = os.path.join(work, "session")
    os.mkdir(folder)
    master, slave = os.openpty()
    # A console-sized screen, so every track's line fits.
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    before = modes(slave)
    failures = []
    again = None
    with open(log_path, "wb") as log:
        screen = Screen(master, log)
        common = ["--dir", folder, "--tracks", str(args.tracks),
                  "--backend", "fake", "--no-memlock", "--plugin-cache", "",
                  "--autosave-seconds", "0"]
        pid = start_engine(args.engine, common + ["--arm-all"], slave,
                           args.fsize)

        def abandon(why):
            failures.append(why)
            kill_all(session_pids(pid))
            wait_exit(pid, screen, 5)

        try:
            if not screen.wait_for(rb"SONORA .*take 1", 0, 20):
                abandon("the interface never drew its status line")
                return failures
            since = screen.mark()
            os.write(master, b" ")
            if not screen.wait_for(rb"REC ", since, 5):
                abandon("the take never started recording")
                return failures
            if args.fsize > 0:
                if not screen.wait_for(FAILURE_RE, since, 30):
                    abandon("the interface never showed the take failing")
                    return failures
            else:
                # The control: no limit, so the take must not fail.
                screen.pump(2)

            if mode == "key":
                os.write(master, b"q")
            elif mode == "engine":
                os.kill(pid, signal.SIGTERM)
            else:
                for p in session_pids(pid):
                    try:
                        os.kill(p, signal.SIGTERM)
                    except OSError:
                        pass  # Gone between the listing and the signal.
            code = wait_exit(pid, screen, STOP_SECONDS)
            if code is None:
                abandon(f"the engine had not exited {STOP_SECONDS} s after the quit")
                return failures
            print(f"engine exited {code}")
            if code != 1:
                failures.append(f"exit {code}, not 1")
            if not re.search(rb"take failed, ", bytes(screen.data)):
                failures.append("the engine never said on the terminal why the take failed")
            # Whatever is left on the terminal: an interface that outlived
            # the engine would still hold it.
            left = [p for p in session_pids(pid) if p != pid]
            if left:
                failures.append(f"processes left on the terminal: {left}")
            after = modes(slave)
            if after != before:
                failures.append("terminal not restored: " + describe_modes(before, after))
            else:
                print("terminal modes read back as before")

            takes = sorted(
                os.path.join(d, f) for d, _, files in os.walk(folder)
                for f in files if f.endswith(".wav"))
            if len(takes) != args.tracks:
                failures.append(f"{len(takes)} take files, not {args.tracks}")
            known = []
            for t in takes:
                is_known, why = check_take(t)
                name = os.path.relpath(t, folder)
                if is_known:
                    known.append(f"{name}: {why}")
                elif why:
                    failures.append(f"{name}: {why}")
            print(f"{len(takes)} take files checked")
            if known:
                more = f" (and {len(known) - 1} more)" if len(known) > 1 else ""
                print(f"xfail ({mode}): {known[0]}{more}")

            # The unit restarts the engine into the same folder.
            since = screen.mark()
            again = start_engine(args.engine, common + ["--headless"], slave, 0)
            if not screen.wait_for(rb"engine listening on", since, 20):
                failures.append("the folder did not open again")
            os.kill(again, signal.SIGTERM)
            code = wait_exit(again, screen, 20)
            if code is not None:
                again = None
            if code != 0:
                failures.append(f"reopened engine exited {code}, not 0")
            else:
                print("the folder opened again")
        finally:
            kill_all(session_pids(pid) + (session_pids(again) if again else []))
            os.close(master)
            os.close(slave)
            shutil.rmtree(work, ignore_errors=True)
    return failures


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--engine", required=True,
                   help="sonora-engine, with sonora-tui beside it")
    p.add_argument("--mode", action="append",
                   choices=["key", "engine", "session"],
                   help="how to quit (repeatable; default: all three)")
    p.add_argument("--tracks", type=int, default=18)
    # 24-bit mono at 48 kHz is 144 kB a second, so a take fails about 1.5 s
    # in. 0 removes the limit: the control run, which must then fail the
    # exit-code check.
    p.add_argument("--fsize", type=int, default=256 * 1024,
                   help="RLIMIT_FSIZE for the engine, in bytes (0: none)")
    p.add_argument("--logs", default=tempfile.gettempdir(),
                   help="where each run's screen is kept")
    args = p.parse_args()
    # In order with the FAIL lines on stderr when CI captures both.
    sys.stdout.reconfigure(line_buffering=True)
    args.engine = os.path.abspath(args.engine)
    started = time.monotonic()
    failed = 0
    for mode in args.mode or ["key", "engine", "session"]:
        failed += bool(run(args, mode))
    print(f"{'FAIL' if failed else 'PASS'} in {time.monotonic() - started:.1f} s")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

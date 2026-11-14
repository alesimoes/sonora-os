#!/usr/bin/env python3
"""Start a command detached from its caller and print its pid.

    python tools/harness/detach.py --cwd DIR --out FILE -- COMMAND [ARG...]

tools/build-job.sh uses it to run a build that must outlive the command that
started it: an agent's tool call is cut at about ten minutes, and on this
Windows host a UI micro build plus its wait for the heavy lock is longer
(2026-11-13). The child gets no console, its own process group, and, where
the caller sits in a Windows job object that allows it, leaves that job, so
the tree kill at the end of the caller's call does not reach it. stdin is
closed and stdout/stderr go to FILE: an inherited pipe would hold the
caller's call open until the build ends.
"""
import argparse
import os
import subprocess
import sys

NEW_GROUP = 0x00000200       # CREATE_NEW_PROCESS_GROUP
NO_WINDOW = 0x08000000       # CREATE_NO_WINDOW
BREAKAWAY = 0x01000000       # CREATE_BREAKAWAY_FROM_JOB


def spawn(argv, cwd, out, popen=subprocess.Popen, nt=None):
    """Starts argv detached; returns the Popen. A job that refuses breakaway
    (ERROR_ACCESS_DENIED) gets the child without it."""
    nt = (os.name == "nt") if nt is None else nt
    with open(out, "ab") as f:
        kw = dict(cwd=cwd, stdin=subprocess.DEVNULL, stdout=f,
                  stderr=subprocess.STDOUT, close_fds=True)
        if not nt:
            return popen(argv, start_new_session=True, **kw)
        try:
            return popen(argv, creationflags=NEW_GROUP | NO_WINDOW | BREAKAWAY,
                         **kw)
        except OSError:
            return popen(argv, creationflags=NEW_GROUP | NO_WINDOW, **kw)


def main(args=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cwd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("argv", nargs=argparse.REMAINDER)
    a = ap.parse_args(args)
    argv = a.argv[1:] if a.argv[:1] == ["--"] else a.argv
    if not argv:
        ap.error("no command")
    print(spawn(argv, a.cwd, a.out).pid)
    return 0


if __name__ == "__main__":
    sys.exit(main())

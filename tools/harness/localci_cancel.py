"""Dropping local-ci work whose PRs are all merged or closed (2026-11-06: a
batch kept building for ~2 h after #444 was merged by hand). A cancelled run
is never a pass: its state is `cancelled` and passed() needs the script's own
"local-ci: all passed" line, which a killed run never writes."""
import os
import signal
import subprocess
import sys


def kill_tree(pid):
    """Stops the detached shell and everything it started (builds, docker
    clients). Best effort: the process may already be gone."""
    if not pid:
        return
    try:
        if sys.platform == "win32":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                           capture_output=True, timeout=30)
        else:
            os.killpg(pid, signal.SIGTERM)
    except (OSError, subprocess.SubprocessError):
        pass


def mark_cancelled_files(logdir):
    """rc and summary say cancelled, so nobody reads a stale run as a pass."""
    try:
        with open(os.path.join(logdir, "summary"), "w") as f:
            f.write("local-ci: cancelled (its PRs are merged or closed)\n")
        with open(os.path.join(logdir, "rc"), "w") as f:
            f.write("143\n")
    except OSError:
        pass


def prune_queue(items, head_of, gone):
    """(kept, dropped heads) for a queue of entries."""
    kept, dropped = [], []
    for it in items:
        (dropped if gone(head_of(it)) else kept).append(it)
    return kept, [head_of(i) for i in dropped]

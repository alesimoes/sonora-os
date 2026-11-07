"""Dropping local-ci work whose heads nobody awaits any more (2026-11-06: a
batch kept building for ~2 h after #444 was merged by hand; 2026-11-07: the
one builder spent ~5 h on the head of an already merged PR). A head is gone
when its PR is merged or closed, or when a fixer push or a refresh replaced
it. It leaves every queue: the pool, the bisect groups and a running run.
A cancelled run is never a pass: its state is `cancelled` and passed() needs
the script's own "local-ci: all passed" line, which a killed run never
writes."""
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


def prune(ci, gone):
    """Drops every gone head from the queues of `ci` (a LocalCI). A running
    run with even one gone head is killed, process group and all: its
    candidate merges a head nobody will merge, so its verdict is worth
    nothing. Its live heads go back, as one group at the front, to be built
    again without the gone ones."""
    pool = ci._queue("pool")
    kept, dropped = prune_queue(pool, lambda e: e["head"], gone)
    if dropped:
        ci._set_queue("pool", kept)
        ci._cancel_heads(dropped)
    groups, left, dropped = ci._queue("groups"), [], []
    for g in groups:
        live = [h for h in g if not gone(h)]
        dropped += [h for h in g if h not in live]
        if live:
            left.append(live)
    if dropped:
        ci._set_queue("groups", left)
        ci._cancel_heads(dropped)
    for key, r in ci._running():
        heads = r.get("heads") or [key]
        dead = [h for h in heads if gone(h)]
        if not dead:
            continue
        kill_tree(r.get("pid"))
        mark_cancelled_files(r.get("dir") or "")
        ci._set(key, state="cancelled", ended=ci.clock())
        ci._cancel_heads(dead)
        live = [h for h in heads if h not in dead and h != key]
        if live:
            ci._set_queue("groups", [live] + ci._queue("groups"))
            for h in live:
                ci._set(h, state="running", queued=True, batch=None)
            ci.o.store.event("local-ci", "local_ci_requeued",
                             ",".join(h[:8] for h in live))

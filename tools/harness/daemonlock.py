"""Who may write state.json (2026-11-07: CLI adoptions vanished).

The daemon keeps state.json in memory and rewrites the whole file on every
mutation, so a second Orchestrator opened by a CLI command (adopt, add,
import-queue, bundle) wrote its change and the daemon's next save erased it:
`adopted_pr` events existed for #471-#473, their features did not.

One writer at a time, by a lock file `.harness/daemon.lock` (pid, role,
start): the daemon takes it for its whole run; a CLI command that would
write the store takes it only when no daemon holds it, and otherwise posts
its request to the daemon (harness/cli_mailbox.py). A lock whose pid is dead
is stale and taken over. The status.json age (autoadopt.daemon_alive) stays
a second sign, for a daemon started before this lock existed.
"""
import json
import os
import time

from .launcher import pid_alive

LOCK = "daemon.lock"


class Busy(Exception):
    """Another live process holds the lock."""


def path(hdir):
    return os.path.join(hdir, LOCK)


def holder(hdir, alive=pid_alive):
    """{'pid', 'role', 'started'} of the live holder, or None."""
    try:
        with open(path(hdir), encoding="utf-8") as f:
            h = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(h, dict) or not alive(h.get("pid")):
        return None
    return h


def daemon_running(hdir, alive=pid_alive):
    h = holder(hdir, alive)
    return bool(h and h.get("role") == "daemon")


def _create(hdir, role, pid):
    os.makedirs(hdir, exist_ok=True)
    fd = os.open(path(hdir), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"pid": pid, "role": role, "started": time.time()}, f)


def acquire(hdir, role, wait_s=0.0, alive=pid_alive, sleep=time.sleep,
            pid=None):
    """Takes the lock for `role` ('daemon' or 'cli'). A CLI command waits up
    to wait_s for a CLI holder; a daemon waits for a CLI command to finish
    (a re-exec'd daemon meets the one that slipped in while it restarted).
    Raises Busy when a live holder keeps it."""
    pid = pid or os.getpid()
    end = time.time() + wait_s
    while True:
        try:
            _create(hdir, role, pid)
            return
        except FileExistsError:
            pass
        h = holder(hdir, alive)
        if h is None:
            try:            # stale: its process is gone
                os.remove(path(hdir))
            except OSError:
                pass
            continue
        if h.get("pid") == pid:
            return          # ours already (a re-exec on POSIX keeps the pid)
        if h.get("role") == "daemon" or time.time() >= end:
            raise Busy(f"{h.get('role')} pid {h.get('pid')} holds "
                       f"{path(hdir)}")
        sleep(0.2)


def release(hdir, pid=None):
    """Removes the lock if this process holds it."""
    pid = pid or os.getpid()
    try:
        with open(path(hdir), encoding="utf-8") as f:
            if json.load(f).get("pid") != pid:
                return
        os.remove(path(hdir))
    except (OSError, ValueError, AttributeError):
        pass

"""Screenshot regeneration as the daemon's own job (2026-11-08).

`tools/build-slot.sh tools/build-ui.sh --screenshot` can outlive an agent's
10-minute command limit, so UI tasks with pictures ended blocked with their
code done. The engineer commits code only; before the PR is opened the daemon
starts the regeneration detached (nobody waits on it), polls it each tick,
commits docs/images to the feature branch and then lets the PR open.

Per feature, `shots:<fid>` in the store holds
  state      running | done | failed
  head       the branch head the run started from
  done_head  the head once the pictures were committed (state done)
A branch whose diff against main touches no UI source needs no pictures.
"""
import json
import os

from . import proctree
from .shots_io import RealIO

READY, WAIT, FAILED = "ready", "wait", "failed"
MAX_TRIES = 2
RETRY_S = 300
JOB_MAX_S = 3 * 3600


def needs_pictures(files):
    """UI source changed (tests and prose do not move a picture)."""
    for f in files:
        if not f.startswith("ui/") or f.endswith(".md"):
            continue
        if "/tests/" in f or "/test/" in f:
            continue
        return True
    return False


class Shots:
    def __init__(self, orch, io=None):
        self.o = orch
        self.io = io or RealIO()

    def info(self, fid):
        try:
            return json.loads(self.o.store.get_meta(f"shots:{fid}") or "{}")
        except ValueError:
            return {}

    def _save(self, fid, st):
        self.o.store.set_meta(f"shots:{fid}", json.dumps(st))

    def _paths(self, fid):
        d = os.path.join(self.o.hdir, "shots")
        os.makedirs(d, exist_ok=True)
        return os.path.join(d, f"{fid}.log"), os.path.join(d, f"{fid}.rc")

    def tail(self, fid, n=1500):
        try:
            with open(self._paths(fid)[0], errors="replace") as f:
                return f.read()[-n:]
        except OSError:
            return ""

    def step(self, fid, worktree, task_id, now):
        """READY: open the PR. WAIT: the job runs or backs off. FAILED:
        out of tries; the caller blocks the feature."""
        st = self.info(fid)
        head = self.io.head(worktree)
        if st.get("state") == "done" and st.get("done_head") == head:
            return READY
        if st.get("state") == "running":
            return self._poll(fid, worktree, task_id, st, now)
        if not needs_pictures(self.io.changed(worktree)):
            return READY
        if st.get("state") == "failed":
            if st.get("tries", 0) >= MAX_TRIES:
                return FAILED
            if now - st.get("ended", 0) < RETRY_S:
                return WAIT
        return self._start(fid, worktree, task_id, st, head, now)

    def _start(self, fid, worktree, task_id, st, head, now):
        log, rc = self._paths(fid)
        if os.path.exists(rc):
            os.remove(rc)
        pid = self.io.start(worktree, log, rc)
        tries = 0 if st.get("state") == "done" else st.get("tries", 0)
        self._save(fid, {"state": "running", "head": head, "pid": pid,
                         "started": now, "tries": tries + 1})
        self.o.store.event(task_id, "shots_started", f"pid {pid}")
        return WAIT

    def _poll(self, fid, worktree, task_id, st, now):
        rc = self._exit_code(fid)
        if rc is None:
            if not self.io.alive(st.get("pid")):
                return self._failed(fid, task_id, st, now,
                                    "the job died without an exit code")
            if now - st.get("started", now) > JOB_MAX_S:
                proctree.kill_tree(st.get("pid"))
                return self._failed(fid, task_id, st, now,
                                    "the job ran past its time limit")
            return WAIT
        if rc != 0:
            return self._failed(fid, task_id, st, now, f"exit code {rc}")
        try:
            committed = self.io.commit(worktree)
        except RuntimeError as e:
            return self._failed(fid, task_id, st, now, str(e))
        st.update(state="done", done_head=self.io.head(worktree), ended=now)
        self._save(fid, st)
        self.o.store.event(task_id, "shots_done",
                           "pictures committed" if committed else "unchanged")
        return READY

    def _exit_code(self, fid):
        try:
            with open(self._paths(fid)[1]) as f:
                return int(f.read().strip())
        except (OSError, ValueError):
            return None

    def _failed(self, fid, task_id, st, now, why):
        st.update(state="failed", ended=now)
        self._save(fid, st)
        self.o.store.event(task_id, "shots_failed", why)
        return WAIT if st.get("tries", 0) < MAX_TRIES else FAILED

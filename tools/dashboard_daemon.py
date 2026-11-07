"""Is the orchestrator daemon running? Three signs, any one is enough:
.harness/daemon.lock naming a live pid with role daemon, status.json written
in the last two minutes, events.jsonl written in the last two minutes. Status
and events live under planning/orchestrator/ on main (an earlier reading of
the old path wrongly called pid 9420 dead)."""
import json
import os
import time

from dashboard_util import orch_root, orch_status_path
from harness import launcher

FRESH_S = 120.0


def _fresh(path, now):
    try:
        return now - os.path.getmtime(path) < FRESH_S
    except OSError:
        return False


def lock_pid(root=None):
    """The live daemon pid from .harness/daemon.lock, or None."""
    try:
        with open(os.path.join(root or orch_root(), ".harness", "daemon.lock"), encoding="utf-8") as f:
            h = json.load(f)
    except (OSError, ValueError):
        return None
    if isinstance(h, dict) and h.get("role") == "daemon" and launcher.pid_alive(h.get("pid")):
        return h["pid"]
    return None


def daemon_alive(now=None):
    now = now or time.time()
    status = orch_status_path()
    return bool(lock_pid() or _fresh(status, now)
                or _fresh(os.path.join(os.path.dirname(status), "events.jsonl"), now))

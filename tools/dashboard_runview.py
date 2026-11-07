"""What the dashboard adds to the daemon's status.json so the owner sees at
once what a run is doing and which pools are paused: the last step from the
run's stream log, and the live paused.json (status.json lags one tick)."""
import json
import os

from dashboard_runs import run_activity
from dashboard_util import orch_root


def last_step(stream_path):
    """One line: the newest thing the agent said or did, or ''."""
    if not stream_path:
        return ""
    items = run_activity(stream_path, 1)
    if not items:
        return ""
    it = items[-1]
    text = " ".join(str(it["text"]).split())
    return ("%s: %s" % (it["kind"], text))[:240]


def add_last_steps(o):
    if not isinstance(o, dict):
        return o
    o = dict(o)
    o["runs"] = [dict(r, last_step=last_step(r.get("stream_path"))) for r in o.get("runs") or []]
    return o


def live_paused():
    """The pools paused right now, or None when paused.json is unreadable."""
    path = os.path.join(orch_root(), "planning", "orchestrator", "paused.json")
    try:
        with open(path, encoding="utf-8") as f:
            v = json.load(f)
    except FileNotFoundError:
        return []
    except (OSError, ValueError):
        return None
    return sorted(str(x) for x in v) if isinstance(v, list) else None


def with_live_paused(o):
    """Copy of the orchestrator view whose paused list is current."""
    if not isinstance(o, dict):
        return o
    paused = live_paused()
    if paused is None:
        return o
    o = dict(o)
    o["paused"] = paused
    pools = {}
    for n, p in (o.get("pools") or {}).items():
        pools[n] = dict(p, paused=n in paused) if isinstance(p, dict) else p
    o["pools"] = pools
    return o

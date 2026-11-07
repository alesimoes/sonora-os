"""What the dashboard reads from the orchestrator: status.json (parsed in one
place), state.json, the coordinator's note and main's progress catalogue."""
import json
import os
import re
import time

from dashboard_util import orch_root, orch_status_path, run


def read_orchestrator():
    """status.json when the daemon writes it; None otherwise."""
    try:
        with open(orch_status_path(), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def pr_of(task):
    m = re.match(r"pr-(\d+)$", str(task or ""))
    return int(m.group(1)) if m else None


def failed_steps(row):
    """Names of the PR's failed GitHub jobs, as the merge train reports them."""
    return [n for n in (row or {}).get("failing") or [] if isinstance(n, str)]


def clean_orch(o, open_prs):
    """Drop what is no longer current: closed PRs, PR-less queue entries and
    runs that stopped giving signs of life. open_prs is None when GitHub
    could not be read, and then no PR is judged closed."""
    if not isinstance(o, dict):
        return o
    o = dict(o)
    now = time.time()
    if open_prs is not None:
        def gone(item):
            n = pr_of(item.get("task")) if isinstance(item, dict) else None
            return n is not None and n not in open_prs
        o["train"] = [r for r in o.get("train") or [] if r.get("state") == "merged" or r.get("pr") in open_prs]
        o["blocked"] = [b for b in o.get("blocked") or [] if not gone(b)]
        o["waits_on_owner"] = [b for b in o.get("waits_on_owner") or [] if not gone(b)]
        o["features"] = [f for f in o.get("features") or []
                         if not (f.get("pr") and f["pr"] not in open_prs and f.get("state") != "merged")]
        b = dict(o.get("builder") or {})
        q = dict(b.get("queue") or {})
        seen = []
        for n in q.get("prs") or []:
            if n in open_prs and n not in seen:
                seen.append(n)
        q["open_prs"] = seen
        b["queue"] = q
        o["builder"] = b
    if o.get("train"):
        o["train"] = [dict(r, failed_steps=failed_steps(r)) for r in o["train"]]
    o["runs"] = [r for r in o.get("runs") or [] if now - (r.get("last_beat") or r.get("started") or now) < 1800]
    return o


def read_coordinator():
    """What the coordinator session (Claude in the terminal) says it is doing:
    <orchestrator root>/.harness/coordinator.json, written by the session."""
    try:
        with open(os.path.join(orch_root(), ".harness", "coordinator.json"), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def read_progress():
    """How much of the project is done, from main's catalogue
    (planning/progress.json on origin/main, tallied by tools/progress.py):
    only merged and verified work counts, as in planning/PROGRESS.md."""
    import importlib.util
    root = orch_root()
    run(["git", "-C", root, "fetch", "-q", "origin", "main"], timeout=60)
    raw = run(["git", "-C", root, "show", "origin/main:planning/progress.json"])
    spec = importlib.util.spec_from_file_location(
        "sonora_progress", os.path.join(os.path.dirname(os.path.abspath(__file__)), "progress.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    overall, fronts, _ = mod.tally(json.loads(raw))
    one = lambda t: {"pct": t.pct(), "done": t.done, "total": t.total, "doing": t.doing}
    return dict(one(overall), fronts=[dict(one(t), title=f["title"]) for f, t, _ in fronts])


def orch_state():
    try:
        with open(os.path.join(orch_root(), "planning", "orchestrator", "state.json"), encoding="utf-8") as f:
            s = json.load(f)
    except Exception:
        return {}
    for k in ("tasks", "features", "runs"):
        v = s.get(k) or []
        s[k] = list(v.values()) if isinstance(v, dict) else v
    return s

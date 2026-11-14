"""What the dashboard reads from the orchestrator: status.json (parsed in one
place) and the coordinator's subagent file."""
import json
import os
import re
import time

from dashboard_util import main_root, orch_root, orch_status_path

AGENT_STALE_S = 6 * 3600


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
        o["blocked"] = [dict(g, items=[b for b in g.get("items") or [] if not gone(b)])
                        for g in o.get("blocked") or [] if isinstance(g, dict)]
        o["blocked"] = [g for g in o["blocked"] if g["items"]]
        o["waits_on_owner"] = [b for b in o.get("waits_on_owner") or [] if not gone(b)]
        o["features"] = [f for f in o.get("features") or []
                         if not (f.get("pr") and f["pr"] not in open_prs and f.get("state") != "merged")]
    if o.get("train"):
        o["train"] = [dict(r, failed_steps=failed_steps(r)) for r in o["train"]]
    o["runs"] = [r for r in o.get("runs") or [] if now - (r.get("last_beat") or r.get("started") or now) < 1800]
    return o


def read_coordinator_agents(now=None):
    """The coordinator's own subagents, as the hooks of harness/coord_hook.py
    record them: <root>/.harness/coordinator-agents.json under the
    orchestrator's checkout and under the main checkout, merged by id."""
    now = now or time.time()
    rows = {}
    for base in dict.fromkeys([orch_root(), main_root()]):
        try:
            with open(os.path.join(base, ".harness", "coordinator-agents.json"), encoding="utf-8") as f:
                got = (json.load(f).get("agents") or {})
        except (OSError, ValueError, AttributeError):
            continue
        for k, a in got.items():
            if isinstance(a, dict) and now - (a.get("last") or 0) < AGENT_STALE_S:
                rows.setdefault(k, a)
    return list(rows.values())

"""What the previous run leaves the next one (2026-11-08 audit).

The hub carries messages between live agents, but consecutive roles on one
piece of work rarely overlap: the tester starts after the engineer wrote
result.json, the integrator after the testers, a relaunch after a dead run.
Each got the brief, the branch and a `note`, never the last result: summary,
commit, what was left unverified, the findings. This module turns the last
finished result.json of the task (and of its sibling tasks, for the
integrator and the fixer) into a bounded block for the prompt.
"""
import json

from . import heartbeat

MAX_CHARS = 3500
PER_RUN = 1200
MAX_ITEMS = 6
HEAD = ("Handoff from the previous run(s) on this work (their result.json, "
        "newest per task; continue from it, do not redo it):")


def last_result(store, task_id):
    """(run, data) of the newest ended run with a readable result.json."""
    for r in reversed(store.runs_of(task_id)):
        if r.get("ended") is None:
            continue
        data, _ = heartbeat.read_result(r["result_path"])
        if data:
            return r, data
    return None, None


def _item(x):
    return (json.dumps(x, sort_keys=True) if isinstance(x, (dict, list))
            else str(x))


def format_result(r, data, limit=PER_RUN):
    lines = [f"- {r['role']} {r['name']} on {r['task_id']}: "
             f"{data.get('status')}",
             "  summary: " + " ".join((data.get("summary") or "").split())]
    for k in ("commit", "branch"):
        if data.get(k):
            lines.append(f"  {k}: {data[k]}")
    for k in ("unverified", "findings", "followups"):
        items = data.get(k) or []
        if items:
            lines.append(f"  {k}: " + "; ".join(
                _item(i) for i in items[:MAX_ITEMS]))
    return "\n".join(lines)[:limit]


def brief(store, task, siblings=()):
    """Bounded handoff text, or "" when no run has finished yet."""
    parts, seen = [], set()
    for t in [task, *siblings]:
        if t["id"] in seen:
            continue
        seen.add(t["id"])
        r, data = last_result(store, t["id"])
        if data:
            parts.append(format_result(r, data))
    if not parts:
        return ""
    return HEAD + "\n" + "\n".join(parts)[:MAX_CHARS]

"""The drawer's content for a clicked card: the coordinator, an agent, one
run, or a feature / PR with its tasks, runs and events."""
from dashboard_chat import NOT_REACHABLE, chat_view
from dashboard_orch import orch_state, read_coordinator
from dashboard_runs import events_of, run_activity, task_view
from dashboard_state import lock, state

RUN_KEYS = ("id", "name", "role", "state", "started", "ended", "exit")


def _agent(ident):
    with lock:
        a = next((x for x in state["agents"] if x["id"] == ident), None)
        lk = list(state.get("lock") or [])
    return {"kind": "agent", "title": (a or {}).get("label", ident), "agent": a, "lock": lk, "not_reachable": NOT_REACHABLE}


def _run(ident, tasks, runs):
    r = next((x for x in runs if x.get("id") == ident), None)
    if not r:
        return {"error": "run não encontrado"}
    t = next((x for x in tasks if x.get("id") == r.get("task_id")), None)
    return {"kind": "run", "title": "%s · %s" % (r.get("name") or r.get("id"), r.get("role") or ""),
            "run": {k: r.get(k) for k in RUN_KEYS + ("last_beat",)},
            "task": task_view(t), "activity": run_activity(r.get("stream_path") or ""),
            "events": events_of({r.get("task_id")}, 10), "chat": chat_view(r)}


def _feature(kind, ident, tasks, feats, runs):
    prnum = ident if kind == "pr" else None
    if kind == "pr":
        f = next((x for x in feats if str(x.get("pr")) == str(ident)), None)
        ident = f.get("id") if f else "pr-%s" % ident
    f = next((x for x in feats if x.get("id") == ident or x.get("name") == ident), None)
    ts = [x for x in tasks if x.get("feature_id") == ident or x.get("id") == ident]
    train = next((x for x in ((state.get("orchestrator") or {}).get("train") or [])
                  if (f and str(x.get("pr")) == str(f.get("pr"))) or (prnum and str(x.get("pr")) == str(prnum))), None)
    pr_row = next((p for p in state.get("prs") or [] if str(p["number"]) == str(prnum)), None) if prnum else None
    rs = [x for x in runs if x.get("task_id") in {t.get("id") for t in ts}]
    return {"kind": "feature", "title": ("PR #%s: %s" % (prnum, (train or pr_row or {}).get("title", ""))) if prnum else ident,
            "gh": pr_row,
            "feature": {k: (f or {}).get(k) for k in ("id", "state", "track", "pr", "branch", "fixes", "partial", "blocked_reason", "job_reds")},
            "train": train, "tasks": [task_view(t) for t in ts][:6],
            "runs": [{k: x.get(k) for k in RUN_KEYS} for x in rs][-6:],
            "events": events_of({ident} | {t.get("id") for t in ts}, 20)}


def detail(kind, ident):
    if kind == "coord":
        return {"kind": "coord", "title": "Coordenador (Claude, sessão no terminal)", "coord": read_coordinator() or {}}
    if kind == "agent":
        return _agent(ident)
    s = orch_state()
    tasks, feats, runs = s.get("tasks", []), s.get("features", []), s.get("runs", [])
    if kind == "run":
        return _run(ident, tasks, runs)
    return _feature(kind, ident, tasks, feats, runs)

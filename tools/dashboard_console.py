"""The console screen's data: status strip, running agents, attention queue
(docs/design/dashboard-console/SPEC.md). Pure functions of the dashboard's
snapshot and a clock, so every rule is unit-tested; nothing here does I/O.
"""
import time

import dashboard_kpis
from dashboard_attention import attention

NO_SIGNAL_S = 120
# The daemon writes status.json once a tick: tick_s (30 s) of sleep plus the
# tick's own work, about 50 s in practice and minutes for a startup sweep.
# 30/120 showed a healthy daemon as amber all the time and as "parado" after
# one slow tick (owner, 2026-11-09: "parece que o daemon está parado").
DAEMON_OK_S, DAEMON_WARN_S = 120, 600


def level_of(n, cap, brake=False):
    """ok under 80 %, warn from 80 %, bad when full (or the brake is on)."""
    if brake or (cap and n >= cap):
        return "bad"
    return "warn" if cap and n >= 0.8 * cap else "ok"


def daemon_cell(orch, now, alive=True):
    """vivo / lento / travado (the process runs but its ticks do not come
    back) / parado (no process)."""
    ts = (orch or {}).get("updated_ts")
    if not isinstance(ts, (int, float)):
        return {"level": "bad", "text": "parado"}
    age = max(0, int(now - ts))
    if age < DAEMON_OK_S:
        return {"level": "ok", "text": f"vivo {age}s"}
    if not alive:
        return {"level": "bad", "text": "parado"}
    if age < DAEMON_WARN_S:
        return {"level": "warn", "text": f"lento {age}s"}
    return {"level": "bad", "text": f"travado {age // 60}min"}


def _cell(n, cap, brake=False):
    return {"n": n, "cap": cap, "level": level_of(n, cap, brake),
            "text": f"{n}/{cap}" if cap else str(n)}


def agent_rows(board_agents, now):
    """One row per running agent, longest running first."""
    out = []
    for a in board_agents or []:
        if a.get("name") == "console":
            continue    # the console's own short run is not a worker
        since = a.get("since")
        last = a.get("last")
        quiet = now - last if isinstance(last, (int, float)) else None
        out.append({"id": a.get("name") or a.get("id"), "model": a.get("model") or "",
                    "task": a.get("what") or "", "role": a.get("role_pt") or "",
                    "pr": a.get("pr"),
                    "minutes": round((now - since) / 60) if isinstance(since, (int, float)) else None,
                    "no_signal_min": round(quiet / 60) if quiet is not None and quiet > NO_SIGNAL_S else None})
    return sorted(out, key=lambda r: -(r["minutes"] or 0))


def build(snap, daemon_alive, now=None):
    """The JSON the screen polls (GET /api/console)."""
    now = now or time.time()
    snap = snap or {}
    orch = snap.get("orchestrator")
    board = snap.get("board") or {}
    o = orch if isinstance(orch, dict) else {}
    daemon = daemon_cell(orch, now, daemon_alive)
    agents = agent_rows(board.get("agents"), now)
    queue, harness = attention(orch, agents, board.get("prs"), daemon, now)
    prs_n, prs_cap = o.get("open_prs") or 0, o.get("max_open_prs") or 0
    return {"now": now, "daemon_alive": bool(daemon_alive),
            "strip": {"daemon": daemon, "agents": _cell(len(agents), o.get("cap") or 0),
                      "runs": _cell(o.get("runs_last_hour") or 0, o.get("max_runs_per_hour") or 0),
                      "prs": _cell(prs_n, prs_cap, brake=bool(prs_cap and prs_n >= prs_cap)),
                      "alerts": {"n": len(queue), "level": "ok" if not queue else
                                 "bad" if queue[0]["level"] == "bad" else "warn", "text": str(len(queue))}},
            "agents": agents, "attention": queue, "harness": harness,
            "kpis": dashboard_kpis.build(snap, now), "cost": snap.get("cost")}

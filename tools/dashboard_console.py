"""The console screen's data: status strip, running agents, attention queue
(docs/design/dashboard-console/SPEC.md). Pure functions of the dashboard's
snapshot and a clock, so every rule is unit-tested; nothing here does I/O.
"""
import time

NO_SIGNAL_S = 120
DAEMON_OK_S, DAEMON_WARN_S = 30, 120
SEVERITY = {"bad": 0, "warn": 1}


def level_of(n, cap, brake=False):
    """ok under 80 %, warn from 80 %, bad when full (or the brake is on)."""
    if brake or (cap and n >= cap):
        return "bad"
    return "warn" if cap and n >= 0.8 * cap else "ok"


def daemon_cell(orch, now):
    ts = (orch or {}).get("updated_ts")
    if not isinstance(ts, (int, float)):
        return {"level": "bad", "text": "parado"}
    age = max(0, int(now - ts))
    level = "ok" if age < DAEMON_OK_S else "warn" if age < DAEMON_WARN_S else "bad"
    return {"level": level, "text": f"vivo {age}s" if level != "bad" else "parado"}


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


def _item(key, level, title, reason, label=None, command=None, age=0):
    return {"key": key, "level": level, "title": title, "reason": reason,
            "button": {"label": label, "command": command} if label else None, "age": age}


def attention(orch, agents, prs, daemon, now):
    """Only what needs the owner; each item goes away when its cause does."""
    items = []
    if daemon["level"] == "bad":
        items.append(_item("daemon", "bad", "Daemon parado", "Sem o daemon o chat só mostra o estado e nada executa."))
    o = orch or {}
    if o.get("kill_switch"):
        items.append(_item("switch", "warn", "Lançamentos parados", f"A chave {o['kill_switch']} está ligada.",
                           "Retomar", "retome os lançamentos"))
    for a in agents:
        if a["no_signal_min"] is not None:
            items.append(_item("sig:" + a["id"], "bad", f"{a['id']} sem sinal",
                               f"Sem sinal há {a['no_signal_min']} min.", f"Reiniciar {a['id']}",
                               f"reinicie o {a['id']}: pare a execução e recoloque a tarefa na fila",
                               a["no_signal_min"]))
    for p in prs or []:
        if p.get("late"):
            items.append(_item(f"pr:{p['number']}", "warn", f"PR #{p['number']} verde sem merge",
                               p.get("late_text") or "Verde há mais de 10 min.", "Retomar fila",
                               f"por que o PR {p['number']} não entrou? adote-o se for o caso",
                               p.get("green_min") or 0))
    for w in o.get("waits_on_owner") or []:
        items.append(_item("owner:" + str(w.get("task")), "warn", str(w.get("task") or "decisão"),
                           str(w.get("title") or "Espera uma decisão sua.")[:160], "Ver a decisão",
                           f"o que preciso decidir sobre {w.get('task')}?"))
    for al in o.get("alerts") or []:
        items.append(_item("alert:" + str(al.get("id")), "bad" if al.get("level") == "alert" else "warn",
                           str(al.get("check") or al.get("id")), str(al.get("text") or "")[:160],
                           age=round((now - (al.get("since") or now)) / 60)))
    return sorted(items, key=lambda i: (SEVERITY[i["level"]], -i["age"]))


def build(snap, daemon_alive, now=None):
    """The JSON the screen polls (GET /api/console)."""
    now = now or time.time()
    snap = snap or {}
    orch = snap.get("orchestrator")
    board = snap.get("board") or {}
    o = orch if isinstance(orch, dict) else {}
    daemon = daemon_cell(orch, now)
    agents = agent_rows(board.get("agents"), now)
    queue = attention(orch, agents, board.get("prs"), daemon, now)
    prs_n, prs_cap = o.get("open_prs") or 0, o.get("max_open_prs") or 0
    return {"now": now, "daemon_alive": bool(daemon_alive),
            "strip": {"daemon": daemon, "agents": _cell(len(agents), o.get("cap") or 0),
                      "runs": _cell(o.get("runs_last_hour") or 0, o.get("max_runs_per_hour") or 0),
                      "prs": _cell(prs_n, prs_cap, brake=bool(prs_cap and prs_n >= prs_cap)),
                      "alerts": {"n": len(queue), "level": "ok" if not queue else
                                 "bad" if queue[0]["level"] == "bad" else "warn", "text": str(len(queue))}},
            "agents": agents, "attention": queue}

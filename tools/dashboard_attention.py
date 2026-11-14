"""Who each alert is for (owner 2026-11-08: 11 items under 'Precisa de você'
and none was the owner's). Pure functions, no I/O.

The owner queue holds only: a decision, a budget or usage limit, the daemon
down (or its restart failed), a destructive action awaiting Confirmar, a late
green PR, the stop switch. Everything the harness handles itself (stalls,
queues, mail, a restart that worked) goes to a collapsed 'harness cuidando'
line; stalls behind one blocked task fold into one line naming the blocker.
"""
import re

SEVERITY = {"bad": 0, "warn": 1}
# The daemon's kill bound is stalled_s (1800 s); only past it, with the agent
# still silent, has the supervisor's own restart failed.
RESTART_FAILED_MIN = 35
OWNER_CHECKS = {"disk", "budget", "usage", "limit"}
# "blocked: Wrote ... waits for ui.x" names the blocker after "waits for", not
# after the first "blocked:". Task ids carry a dot; try the explicit forms first.
BLOCKER = re.compile(r"(?:waits? (?:for|on)|behind|blocker)[: ]+([A-Za-z0-9][\w.\-/]*)", re.I)
BLOCKED_TASK = re.compile(r"blocked[: ]+([A-Za-z0-9]+(?:[.\-/][\w.\-/]*)+)", re.I)


def item(key, level, title, reason, label=None, command=None, age=0):
    return {"key": key, "level": level, "title": title, "reason": reason,
            "button": {"label": label, "command": command} if label else None, "age": age}


def alert_is_owner_only(al):
    """A restarted daemon is news, not a request; a daemon that stayed down is."""
    check, aid = al.get("check") or "", str(al.get("id") or "")
    if check == "daemon":
        return aid != "daemon:restarted" and al.get("level") == "alert"
    return check in OWNER_CHECKS or "usage_limit" in aid


def blocker_of(text):
    m = BLOCKER.search(text or "") or BLOCKED_TASK.search(text or "")
    return m.group(1).rstrip(".,;:)") if m else None


def _fixer_of(blocker, agents):
    for a in agents:
        if blocker and blocker in (a.get("task") or ""):
            return a["id"]
    return None


def fold_harness(alerts, agents):
    """-> {n, groups:[{key,title,reason,n,blocker,fixer}]}, biggest group first."""
    groups = {}
    for al in alerts:
        b = blocker_of(al.get("text")) if al.get("check") == "stall" else None
        g = groups.setdefault(b or "check:" + (al.get("check") or "outros"),
                              {"blocker": b, "ids": [], "texts": []})
        g["ids"].append(str(al.get("id")))
        g["texts"].append(str(al.get("text") or ""))
    out = []
    for key, g in groups.items():
        n, b = len(g["ids"]), g["blocker"]
        fixer = _fixer_of(b, agents)
        if b:
            title = f"{n} tarefa(s) paradas atrás de {b}"
            reason = f"{b} está bloqueada; " + (f"{fixer} está resolvendo." if fixer else "o harness está tratando.")
        else:
            title = g["texts"][0][:120] if n == 1 else f"{n} avisos de {key.split(':', 1)[1]}"
            reason = "O harness está cuidando."
        out.append({"key": key, "title": title, "reason": reason, "n": n, "blocker": b, "fixer": fixer})
    return {"n": len(alerts), "groups": sorted(out, key=lambda x: -x["n"])}


def attention(orch, agents, prs, daemon, now):
    """-> (owner items, harness fold); each item goes away when its cause does."""
    items = []
    if daemon["level"] == "bad":
        stuck = daemon["text"].startswith("travado")
        items.append(item("daemon", "bad", "Daemon travado" if stuck else "Daemon parado",
                          "O processo roda, mas o ciclo dele não volta há " + daemon["text"][8:] + "."
                          if stuck else "Sem o daemon o chat só mostra o estado e nada executa."))
    o = orch or {}
    if o.get("kill_switch"):
        items.append(item("switch", "warn", "Lançamentos parados", f"A chave {o['kill_switch']} está ligada.",
                          "Retomar", "retome os lançamentos"))
    for a in agents:
        quiet = a["no_signal_min"]
        if quiet is not None and quiet >= RESTART_FAILED_MIN:
            items.append(item("sig:" + a["id"], "bad", f"{a['id']} sem sinal",
                              f"Sem sinal há {quiet} min e o reinício do supervisor não resolveu.",
                              f"Reiniciar {a['id']}",
                              f"reinicie o {a['id']}: pare a execução e recoloque a tarefa na fila", quiet))
    for p in prs or []:
        if p.get("late"):
            items.append(item(f"pr:{p['number']}", "warn", f"PR #{p['number']} verde sem merge",
                              p.get("late_text") or "Verde há mais de 10 min.", "Retomar fila",
                              f"por que o PR {p['number']} não entrou? adote-o se for o caso",
                              p.get("green_min") or 0))
    for w in o.get("waits_on_owner") or []:
        items.append(item("owner:" + str(w.get("task")), "warn", str(w.get("task") or "decisão"),
                          str(w.get("title") or "Espera uma decisão sua.")[:160], "Ver a decisão",
                          f"o que preciso decidir sobre {w.get('task')}?"))
    mine, theirs = [], []
    for al in o.get("alerts") or []:
        (mine if alert_is_owner_only(al) else theirs).append(al)
    for al in mine:
        items.append(item("alert:" + str(al.get("id")), "bad" if al.get("level") == "alert" else "warn",
                          str(al.get("check") or al.get("id")), str(al.get("text") or "")[:160],
                          age=round((now - (al.get("since") or now)) / 60)))
    return sorted(items, key=lambda i: (SEVERITY[i["level"]], -i["age"])), fold_harness(theirs, agents)

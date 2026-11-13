"""What the daemon's status.json says about why work is idle, beyond the
owner queue (2026-11-13: the console showed no blocked group, no Docker
state and no supervisor alerts). Pure functions, no I/O.

Docker down is its own reason, never an owner pause: the supervisor restarts
it (bounded) and the pools resume by themselves.
"""

CAUSE_LABEL = {
    "retry": "tentar de novo mais tarde",
    "conflict": "conflito com outra feature ou claim",
    "plan": "o plano: escopo, claims ou dependências",
    "code": "o código ou os testes falham",
    "infra": "erro da infraestrutura / bug do harness",
    "other": "sem causa reconhecida: ler a nota",
}
NOTE_MAX = 200


def docker_view(o, now):
    since = o.get("docker_down_since")
    if not isinstance(since, (int, float)):
        return None
    held = [str(h) for h in o.get("docker_held") or []]
    return {"since_min": max(0, round((now - since) / 60)), "held": held,
            "text": "Docker parado há %d min; o harness tenta reiniciar" % max(0, round((now - since) / 60))}


def idle_pools(o):
    """One line per pool that is below its floor, with the reason it gives."""
    out = []
    for name, p in sorted((o.get("pools") or {}).items()):
        if not isinstance(p, dict) or not p.get("floor") or p.get("working", 0) >= p["floor"]:
            continue
        why = "docker" if p.get("docker_down") else "pausa" if p.get("paused") else "fila"
        out.append({"pool": name, "why": why, "reason": p.get("reason") or "",
                    "working": p.get("working", 0), "floor": p["floor"]})
    return out


def blocked_groups(o):
    out = []
    for g in o.get("blocked") or []:
        if not isinstance(g, dict):
            continue
        cause = g.get("cause") if g.get("cause") in CAUSE_LABEL else "other"
        items = [{"task": str(b.get("task") or b.get("branch") or ""),
                  "note": str(b.get("note") or "")[:NOTE_MAX]}
                 for b in g.get("items") or [] if isinstance(b, dict)]
        if items:
            out.append({"cause": cause, "label": CAUSE_LABEL[cause], "n": len(items), "items": items})
    return sorted(out, key=lambda g: -g["n"])


def supervisor_view(o):
    acts = (o.get("supervisor") or {}).get("actions") or []
    return [{"what": str(a.get("what") or ""), "check": str(a.get("check") or ""),
             "reason": str(a.get("reason") or "")[:NOTE_MAX], "ts": a.get("ts")}
            for a in acts[-5:] if isinstance(a, dict)]


def build(orch, now):
    o = orch if isinstance(orch, dict) else {}
    return {"docker": docker_view(o, now), "idle_pools": idle_pools(o),
            "blocked": blocked_groups(o), "supervisor": supervisor_view(o)}

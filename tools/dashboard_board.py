"""Every agent that runs, from both launchers, as one list for the board.

Daemon runs come from status.json (`runs`); the coordinator's come from the
subagents file its hooks keep (harness/coord_agents.py) and from the --agents
label file. A PR with two agents on it from different launchers, or of the
same role, is flagged as duplicate work."""
import re

ROLE_PT = {"engineer": "Engenheiro", "fixer": "Corretor", "fix": "Corretor",
           "tester": "Testes (QA)", "qa": "Testes (QA)", "integrator": "Integrador",
           "merge": "Integrador", "principal": "Engenheiro principal",
           "planner": "Arquiteto", "lead": "Líder", "janitor": "Faxineiro",
           "blocker": "Triagem", "reviewer": "Revisor"}


def role_key(name):
    """principal-2 / principal-engineer / principal -> principal."""
    k = re.sub(r"-\d+$", "", str(name or "").lower())
    return re.sub(r"-engineer$", "", k)


def role_pt(name):
    return ROLE_PT.get(role_key(name)) or str(name or "agente")


def feature_of(run, features):
    if run.get("feature"):
        return run["feature"]
    for f in features:
        if any(t.get("name") == run.get("task") for t in f.get("tasks") or []):
            return f.get("id")
    return None


def pr_for(run, features):
    if run.get("pr"):
        return run["pr"]
    m = re.match(r"pr-(\d+)$", str(run.get("task") or ""))
    if m:
        return int(m.group(1))
    f = next((f for f in features if f.get("id") == feature_of(run, features)), None)
    return (f or {}).get("pr")


def daemon_row(r, features):
    return {"key": "run:" + str(r.get("id")), "launcher": "daemon", "kind": "run",
            "id": r.get("id"), "role": role_key(r.get("role")), "role_pt": role_pt(r.get("role")),
            "name": r.get("name") or r.get("id"), "what": r.get("title") or r.get("task"),
            "feature": feature_of(r, features), "pr": pr_for(r, features),
            "since": r.get("started"), "last": r.get("last_beat"),
            "step": r.get("last_step") or "",
            "state": "sem sinal" if r.get("suspect") else "buildando" if r.get("building") else "trabalhando"}


def hook_row(a):
    return {"key": "coord:" + str(a.get("id")), "launcher": "coordenador", "kind": "agent",
            "id": a.get("id"), "role": role_key(a.get("role")), "role_pt": role_pt(a.get("role")),
            "name": a.get("name") or a.get("role"), "what": a.get("doing"),
            "feature": None, "pr": a.get("pr"), "since": a.get("since"),
            "last": a.get("last"), "step": a.get("step") or "", "state": "trabalhando"}


def label_row(a, now):
    idle = a.get("idle_seconds")
    return {"key": "coord:" + str(a.get("id")), "launcher": "coordenador", "kind": "agent",
            "id": a.get("id"), "role": role_key(a.get("agent")), "role_pt": role_pt(a.get("agent")),
            "name": a.get("label") or a.get("id"), "what": a.get("doing"),
            "feature": a.get("branch"), "pr": a.get("pr"), "since": a.get("since"),
            "last": now - idle if idle is not None else None, "step": "",
            "state": {"building": "buildando", "waiting_lock": "esperando a máquina",
                      "working": "trabalhando"}.get(a.get("state"), "parado")}


def mark_duplicates(rows):
    """Two agents on one PR: different launchers (the daemon's fixer and the
    coordinator's principal) or the same role twice."""
    by_pr = {}
    for r in rows:
        if r.get("pr"):
            by_pr.setdefault(r["pr"], []).append(r)
    for pr, mine in by_pr.items():
        launchers = {r["launcher"] for r in mine}
        roles = [r["role"] for r in mine]
        dup = len(mine) > 1 and (len(launchers) > 1 or len(set(roles)) < len(roles))
        for r in mine:
            r["dup"] = dup
            if dup:
                r["dup_with"] = ["%s (%s)" % (o["name"], o["launcher"]) for o in mine if o is not r]
    return rows


def agent_rows(orch, coord_agents, labelled, now):
    """daemon runs + coordinator subagents + label-file sessions, oldest first."""
    features = (orch or {}).get("features") or []
    rows = [daemon_row(r, features) for r in (orch or {}).get("runs") or []]
    rows += [hook_row(a) for a in coord_agents or []]
    seen = {r["key"] for r in rows}
    rows += [row for row in (label_row(a, now) for a in labelled or []
                             if a.get("state") not in ("done", "paused"))
             if row["key"] not in seen]
    return mark_duplicates(sorted(rows, key=lambda r: r.get("since") or now))

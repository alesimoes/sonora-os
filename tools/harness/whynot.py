"""'Por que não entrou?': the answer for one open PR, computed from the
daemon's exported state (status.json) and the GitHub facts the dashboard
already reads. Pure: no I/O, so every branch is unit-tested.

Owner 2026-11-08: three green PRs sat CLEAN (#519 opened from a sub-branch
and never adopted, #527 still owned by a running principal, #528 red with
its fixer killed by a Docker outage) and nobody could ask the dashboard why.
"""
import re

LIVE_STATES = ("queued", "designing", "building", "bugfix", "qa",
               "integrating")
ADOPT_WAIT_S = 300   # autoadopt cadence (train.LIST_TTL_S)


def _failed(pr):
    cancelled = set(pr.get("cancelled") or [])
    return [n for n in pr.get("failed") or [] if n not in cancelled]


def find_owner(pr, status):
    """Who holds the merge: train row, feature, task or running agent."""
    n, branch = pr["number"], pr.get("branch")
    row = next((r for r in status.get("train") or [] if r.get("pr") == n), None)
    feat = next((f for f in status.get("features") or [] if f.get("pr") == n),
                None)
    tasks = [t for t in status.get("tasks") or []
             if t.get("pr") == n or (branch and t.get("branch") == branch
                                     and not t.get("pr"))]
    ids = {t["id"] for t in tasks}
    if feat:
        ids |= {t["name"] for t in feat.get("tasks") or []}
    runs = [r for r in status.get("runs") or []
            if r.get("task") in ids or r.get("name") == (row or {}).get("fixer")]
    return {"row": row, "feature": feat, "tasks": tasks, "runs": runs,
            "task_ids": sorted(ids)}


def owner_text(own):
    if own["runs"]:
        r = own["runs"][0]
        return "agente rodando: %s (%s)" % (r.get("name"), r.get("role"))
    if own["row"]:
        return "trem de merge (fase: %s)" % (own["row"].get("phase") or "?")
    if own["feature"]:
        return "feature %s (estado: %s)" % (own["feature"].get("id"),
                                            own["feature"].get("state"))
    if own["tasks"]:
        t = own["tasks"][0]
        return "tarefa %s (estado: %s), sem PR registrado" % (t["id"],
                                                               t["state"])
    return "ninguém: nenhuma tarefa é dona deste PR"


def gate_text(pr, row):
    done = row.get("passed") if row else pr.get("passed")
    total = row.get("total") if row and row.get("total") is not None \
        else pr.get("total")
    bad = _failed(pr)
    s = "checks %s/%s" % (done if done is not None else "?", total or 0)
    if bad:
        s += ", falhando: " + ", ".join(bad[:3])
    if pr.get("pending"):
        s += ", rodando: %d" % len(pr["pending"])
    if pr.get("cancelled"):
        s += ", cancelados: %d" % len(pr["cancelled"])
    return s


def _reason_unowned(pr, daemon_up):
    if pr.get("draft"):
        return "rascunho: o daemon só adota quando sair de rascunho"
    if not daemon_up:
        return "ninguém é dono e o daemon não está rodando"
    return ("ninguém é dono; o daemon adota sozinho em até %d min "
            "(ou use Adotar)" % (ADOPT_WAIT_S // 60))


def _reason_owned(pr, own, daemon_up):
    row = own["row"]
    bad = _failed(pr)
    if own["runs"] and not (row and row.get("fixer")):
        r = own["runs"][0]
        return ("o agente %s ainda é dono (tarefa em %s); o trem só age "
                "quando ele terminar" % (r.get("name"),
                                         (own["tasks"] or [{}])[0].get("state")
                                         or (own["feature"] or {}).get("state")))
    if not daemon_up:
        return "o daemon não está rodando; ninguém vai fazer o merge"
    if pr.get("base") and pr["base"] != "main":
        return "base %s: PR empilhado, o trem retargeta para main" % pr["base"]
    if pr.get("merge") == "DIRTY" or pr.get("mergeable") == "CONFLICTING":
        who = row.get("fixer") if row else None
        return ("conflito com a main; %s resolvendo" % who) if who else \
            "conflito com a main e nenhum agente: mande o integrator"
    if bad:
        who = row.get("fixer") if row else None
        return ("checks falharam (%s); %s corrigindo" % (bad[0], who)) if who \
            else "checks falharam (%s) e nenhum agente corrige: mande fixer" \
            % bad[0]
    if pr.get("pending"):
        return "esperando %d check(s) terminarem" % len(pr["pending"])
    if pr.get("cancelled"):
        return "checks cancelados: rerodar o CI"
    if not pr.get("total"):
        return "sem checks ainda"
    if pr.get("merge") in ("BLOCKED", "BEHIND"):
        return "GitHub diz %s: falta check obrigatório ou atualizar com a main" \
            % pr["merge"]
    if not row:
        return "verde, mas fora do trem: adote para o daemon fazer o merge"
    return "verde e limpo; o trem (%s) faz o merge no próximo ciclo" \
        % (row.get("phase") or "?")


def last_event(events, own, n):
    """Newest event about this PR: by its task ids or '#N' in the detail."""
    ids = set(own["task_ids"]) | {"pr-%d" % n}
    if own["feature"]:
        ids.add(own["feature"].get("id"))
    tag = re.compile(r"#%d\b" % n)
    for e in events:    # newest first
        if e.get("task_id") in ids or tag.search(e.get("detail") or ""):
            return {"ts": e.get("ts"), "kind": e.get("kind"),
                    "detail": (e.get("detail") or "")[:160]}
    return None


def explain(pr, status, events, daemon_up):
    own = find_owner(pr, status or {})
    owned = bool(own["row"] or own["feature"] or own["tasks"])
    reason = _reason_owned(pr, own, daemon_up) if owned \
        else _reason_unowned(pr, daemon_up)
    return {"owner": owner_text(own), "on_train": bool(own["row"]),
            "gate": gate_text(pr, own["row"]),
            "merge_state": pr.get("merge") or "?",
            "event": last_event(events or [], own, pr["number"]),
            "reason": reason}

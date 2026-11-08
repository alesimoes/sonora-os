"""The brakes on new work, in plain Portuguese, from status.json: PR limit,
runs per hour, cap, paused pools (and why each build pool is held), kill
switch, usage pause and the supervisor's alerts. One row each:
{"level": "ok|warn|bad", "text": ...}."""
import time

# scheduler.Plan.gaps -> words
REASON_PT = {
    "paused": "fila pausada",
    "cap": "limite de agentes simultâneos atingido",
    "hourly run limit": "limite de execuções por hora atingido",
    "kill switch": "chave de emergência ligada",
    "usage pause": "pausa por consumo da conta",
    "CI budget": "PRs prontos demais esperando o gate (orçamento de CI)",
    "no eligible task": "nenhuma tarefa pronta para essa trilha",
    "files claimed or dependencies pending": "arquivos reservados por outro agente ou dependência pendente",
}
POOL_PT = {"build": "Construção (geral)", "build:daw": "Construção · DAW", "build:os": "Construção · OS",
           "build:plugin": "Construção · plugins", "plan": "Planejamento", "qa": "Testes (QA)",
           "fix": "Correção", "merge": "Integração", "principal": "Engenheiro principal"}


def reason_pt(reason):
    r = str(reason or "")
    if "open PRs: merging first" in r:
        n = r.split(" ", 1)[0]
        return "%s PRs abertos: priorizando merges" % n
    return REASON_PT.get(r, r)


def row(level, text):
    return {"level": level, "text": text}


def pr_limit(o):
    n, mx = o.get("open_prs") or 0, o.get("max_open_prs") or 0
    if mx and n >= mx:
        return row("warn", "%d PRs abertos (limite %d): priorizando merges, nenhuma tarefa nova começa" % (n, mx))
    return row("ok", "%d de %s PRs abertos" % (n, mx or "?"))


def hour_limit(o):
    n, mx = o.get("runs_last_hour") or 0, o.get("max_runs_per_hour") or 0
    if mx and n >= mx:
        return row("warn", "%d execuções na última hora (limite %d): sem lançar novas até a hora virar" % (n, mx))
    return row("ok", "%d de %s execuções na última hora" % (n, mx or "?"))


def cap_row(o):
    n, cap = o.get("live") or 0, o.get("cap") or 0
    if cap and n >= cap:
        return row("warn", "%d de %d agentes simultâneos: sem vaga" % (n, cap))
    return row("ok", "%d de %s agentes simultâneos" % (n, cap or "?"))


def switches(o, now):
    out = []
    if o.get("kill_switch"):
        out.append(row("bad", "Chave de emergência %s ligada: nada novo é lançado" % o["kill_switch"]))
    until = o.get("usage_pause_until")
    if isinstance(until, (int, float)) and until > now:
        out.append(row("warn", "Pausa por consumo da conta por mais %d min" % round((until - now) / 60)))
    gh = o.get("gh_paused_until")
    if isinstance(gh, (int, float)) and gh > now:
        out.append(row("warn", "GitHub pausado (limite de requisições) por mais %d min" % round((gh - now) / 60)))
    return out


def pool_rows(o):
    """Paused pools, and every build pool under its floor with the reason."""
    paused = set(o.get("paused") or [])
    out = []
    for name, p in sorted((o.get("pools") or {}).items()):
        if not isinstance(p, dict):
            continue
        label = POOL_PT.get(name, name)
        if name in paused:
            out.append(row("warn", "%s pausada" % label))
        elif name.startswith("build:") and (p.get("live") or 0) < (p.get("floor") or 0) and p.get("reason"):
            out.append(row("warn", "%s: %d de %d ativos, segurada porque: %s" % (
                label, p.get("live") or 0, p.get("floor"), reason_pt(p["reason"]))))
    return out


def alert_rows(o, now):
    return [row("bad" if a.get("level") == "alert" else "warn",
                "Alerta (%s): %s · há %d min" % (a.get("check") or a.get("id"), a.get("text"),
                                                  round((now - (a.get("since") or now)) / 60)))
            for a in o.get("alerts") or []]


def brakes(o, now=None):
    now = now or time.time()
    if not isinstance(o, dict):
        return [row("bad", "Daemon desligado: ninguém lança nem faz merge")]
    age = now - (o.get("updated_ts") or now)
    out = [row("bad", "Daemon parado há %d min: o painel mostra o último estado" % round(age / 60))] if age > 120 else []
    return out + switches(o, now) + [pr_limit(o), hour_limit(o), cap_row(o)] + pool_rows(o) + alert_rows(o, now)

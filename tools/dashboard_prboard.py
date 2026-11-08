"""Open PRs for the board: gate state, who owns the merge, minutes open.

The gate is GitHub's checks (ADR-0061). The daemon merges a green PR itself;
a red or conflicting one goes to a fixer or the integrator. A PR green for
more than GREEN_LATE_S without a merge is red on the board."""
import time
from datetime import datetime

GREEN_LATE_S = 600


def created_ts(p):
    try:
        return datetime.fromisoformat(str(p["created"]).replace("Z", "+00:00")).timestamp()
    except (KeyError, ValueError):
        return None


def gate(p):
    """(code, texto em português, verde?) from the PR's GitHub checks."""
    failed = [n for n in p.get("failed") or [] if n not in (p.get("cancelled") or [])]
    if p.get("draft"):
        return "rascunho", "rascunho: o autor ainda escreve", False
    if p.get("base") and p["base"] != "main":
        return "base", "base %s: só PR contra a main" % p["base"], False
    if p.get("merge") == "DIRTY" or p.get("mergeable") == "CONFLICTING":
        return "conflito", "conflito com a main", False
    if failed:
        return "vermelho", "falhou: " + ", ".join(failed[:2]), False
    if p.get("pending"):
        return "rodando", "checks rodando: %d de %d" % (p.get("passed", 0), p.get("total", 0)), False
    if not p.get("total"):
        return "rodando", "sem checks ainda", False
    if p.get("cancelled"):
        return "cancelado", "checks cancelados: rodar de novo", False
    if p.get("merge") == "BLOCKED":
        return "bloqueado", "falta check obrigatório ou revisão", False
    return "verde", "verde", True


def merge_owner(code, train):
    """Who moves the PR forward, in words."""
    t = train or {}
    if code == "verde":
        return "daemon (merge automático)"
    if code in ("vermelho", "conflito", "cancelado"):
        who = t.get("fixer")
        if who:
            return "%s (%s)" % (who, "integrador" if code == "conflito" else "corretor")
        return "ninguém ainda: falta corretor" if code != "conflito" else "ninguém ainda: falta integrador"
    if code == "rascunho":
        return "autor"
    if code == "rodando":
        return "daemon, quando ficar verde"
    return "ninguém: precisa de ação"


class GreenClock:
    """When each PR was first seen green; forgotten when it stops being green."""
    def __init__(self):
        self.since = {}

    def update(self, number, green, now):
        if not green:
            self.since.pop(number, None)
            return None
        return self.since.setdefault(number, now)


def pr_rows(prs, orch, clock, now=None):
    now = now or time.time()
    train = {t.get("pr"): t for t in (orch or {}).get("train") or []}
    out = []
    for p in prs or []:
        code, text, green = gate(p)
        t = train.get(p["number"])
        gsince = clock.update(p["number"], green, now)
        opened = created_ts(p)
        late = gsince is not None and now - gsince > GREEN_LATE_S
        out.append({"number": p["number"], "title": p.get("title"), "gate": code, "gate_text": text,
                    "owner": merge_owner(code, t), "opened_min": round((now - opened) / 60) if opened is not None else None,
                    "green_min": round((now - gsince) / 60) if gsince is not None else None,
                    "late": late,
                    "late_text": "verde há %d min sem merge: o daemon devia ter feito o merge" % round((now - gsince) / 60) if late else ""})
    return out

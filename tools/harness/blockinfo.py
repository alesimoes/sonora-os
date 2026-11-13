"""Why a task is blocked, and who has to act (owner 2026-11-07).

Only a task its blocker marked `owner` (a decision with no recommended
default, or hardware) waits on the owner. Every other blocked task is
grouped by cause so the dashboard can say "stuck, not you"."""
import re

# cause -> (label shown in Portuguese, sort order)
CAUSES = {
    "retry": ("tentar de novo mais tarde", 0),
    "conflict": ("conflito com outra feature ou claim", 1),
    "plan": ("o plano: escopo, claims ou dependências", 2),
    "code": ("o código ou os testes falham", 3),
    "infra": ("erro da infraestrutura / bug do harness", 4),
    "other": ("sem causa reconhecida: ler a nota", 5),
}
DEFAULT_CAUSE = "other"

# The blockers' own wording, most specific first. 2026-11-13: every block an
# agent reported was stored as "infra" (orchestrator._on_agent_blocked), so
# 28 of 28 blocked items read "infrastructure" on the dashboard, plan
# overlaps, owner decisions and failing tests included.
_INFER = (
    ("retry", re.compile(r"container|cancelled|canceled|try again|"
                         r"retry later", re.I)),
    ("infra", re.compile(
        r"screenshot regeneration failed|docker|WinError|broker|"
        r"time ?limit|timed? ?out|\b\d+ ?s (cap|timeout)|command limit|"
        r"relaunches spent|rate.limit|could not be opened", re.I)),
    ("conflict", re.compile(r"overlap|conflict|held by|claimed by", re.I)),
    ("plan", re.compile(
        r"outside (my|its|the|this|our) (claim|files)|\bscope\b|"
        r"forbidden edge|check-deps|module-deps|needs? the plan|"
        r"plan (must|has to|needs to)", re.I)),
    ("code", re.compile(r"\bfail(s|ed|ing|ures?)?\b|\bred\b|"
                        r"compile error|does not (build|compile)", re.I)),
)
TERMINAL_FEATURE = ("merged", "closed")


def classify(note):
    """The cause the note's wording names, or DEFAULT_CAUSE."""
    for name, rx in _INFER:
        if rx.search(note or ""):
            return name
    return DEFAULT_CAUSE


def cause_of(task):
    """An explicit cause wins, except the blanket "infra" older daemons
    stored for every agent-reported block: there the note decides."""
    c = task.get("cause")
    if c in CAUSES and c != "infra":
        return c
    inferred = classify(task.get("note"))
    if inferred != DEFAULT_CAUSE or c not in CAUSES:
        return inferred
    return c


def superseded(task, features):
    """A blocked task whose feature already merged or closed (PR adopted,
    main green) needs nobody. An owner decision under a partial feature
    that merged is still open: the part left out waits on it."""
    f = features.get(task.get("feature_id"))
    if not (f and f.get("state") in TERMINAL_FEATURE):
        return False
    return not (task.get("owner") and f.get("state") == "merged"
                and f.get("partial"))


def owner_items(tasks, features):
    return [{"title": f"{t['id']}: {t['note'] or t.get('owner') or 'blocked'}",
             "task": t["id"], "branch": t["branch"], "note": t["note"]}
            for t in tasks if t["state"] == "blocked" and t.get("owner")
            and not superseded(t, features)]


def blocked_groups(tasks, features):
    """Blocked tasks that do not need the owner, grouped by cause."""
    groups = {}
    for t in tasks:
        if t["state"] != "blocked" or t.get("owner") \
                or superseded(t, features):
            continue
        groups.setdefault(cause_of(t), []).append(
            {"task": t["id"], "branch": t["branch"], "note": t["note"]})
    return [{"cause": c, "label": CAUSES[c][0], "items": groups[c]}
            for c in sorted(groups, key=lambda c: CAUSES[c][1])]


_BUSY = re.compile(r"container|build (is )?running|in use", re.I)


def is_retry_later(role, summary):
    """The janitor refuses to clean while a build container runs: that is
    the machine being busy, not a block."""
    return role == "janitor" and bool(_BUSY.search(summary or ""))

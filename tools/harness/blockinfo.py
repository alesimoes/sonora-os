"""Why a task is blocked, and who has to act (owner 2026-11-07).

Only a task its blocker marked `owner` with a real ruling (owner_route: a
decision with no recommended default; hardware runs are parked since
2026-11-13) waits on the owner. Every other blocked task is grouped by
cause so the dashboard can say "stuck, not you"."""
import re

from . import owner_route

# cause -> (label shown in Portuguese, sort order)
CAUSES = {
    "retry": ("tentar de novo mais tarde", 0),
    "conflict": ("conflito com outra feature ou claim", 1),
    "plan": ("o plano: escopo, claims ou dependências", 2),
    "code": ("o código ou os testes falham", 3),
    "infra": ("erro da infraestrutura / bug do harness", 4),
    "other": ("sem causa reconhecida: ler a nota", 5),
    # Owner 2026-11-13: hardware-only validations wait until the product is
    # complete; nothing to act on now, for the owner or anyone.
    owner_route.HARDWARE: ("validação em hardware, estacionado", 6),
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


def waits_on_owner(task):
    """Its `owner` names a real owner ruling (owner_route), not hardware,
    the host or a plan step worded as one."""
    return bool(task.get("owner")) and owner_route.cause(
        task["owner"], task.get("note")) is None


def kept_open(task):
    """A blocked slice that stays as it is when its feature merges or is
    carried: the owner's part, or a hardware run parked for later."""
    return task["state"] == "blocked" and bool(
        task.get("owner") or task.get("cause") == owner_route.HARDWARE)


def cause_of(task):
    """An explicit cause wins, except the blanket "infra" older daemons
    stored for every agent-reported block: there the note decides. An
    `owner` that is not the owner's names its own cause."""
    routed = owner_route.cause(task.get("owner"), task.get("note"))
    if routed:
        return routed
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
            for t in tasks if t["state"] == "blocked" and waits_on_owner(t)
            and not superseded(t, features)]


def blocked_groups(tasks, features):
    """Blocked tasks that do not need the owner, grouped by cause."""
    groups = {}
    for t in tasks:
        if t["state"] != "blocked" or waits_on_owner(t) \
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

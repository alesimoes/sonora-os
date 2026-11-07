"""Why a task is blocked, and who has to act (owner 2026-11-07).

Only a task its blocker marked `owner` (a decision with no recommended
default, or hardware) waits on the owner. Every other blocked task is
grouped by cause so the dashboard can say "stuck, not you"."""
import re

# cause -> (label shown in Portuguese, sort order)
CAUSES = {
    "retry": ("tentar de novo mais tarde", 0),
    "conflict": ("conflito", 1),
    "infra": ("erro da infraestrutura / bug do harness", 2),
}
DEFAULT_CAUSE = "infra"

# Notes written before a task carried an explicit cause (older daemon
# state): the same wording the blockers use.
_INFER = (
    ("retry", re.compile(r"container|cancelled|canceled|try again", re.I)),
    ("conflict", re.compile(r"overlap|conflict", re.I)),
)
TERMINAL_FEATURE = ("merged", "closed")


def cause_of(task):
    c = task.get("cause")
    if c in CAUSES:
        return c
    note = task.get("note") or ""
    for name, rx in _INFER:
        if rx.search(note):
            return name
    return DEFAULT_CAUSE


def superseded(task, features):
    """A blocked task whose feature already merged or closed (PR adopted,
    main green) needs nobody."""
    f = features.get(task.get("feature_id"))
    return bool(f and f.get("state") in TERMINAL_FEATURE)


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

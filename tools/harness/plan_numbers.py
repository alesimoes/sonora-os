"""Version numbers a plan needs, reserved when the plan is ingested.

A feature's numbers were reserved only from the `wants` it was opened with.
A plan that turned out to change kProtocolVersion or kFormatVersion
(ui.mixer.sidechain, 2026-11-12) had no reservation, so pre-pr.sh refused
the bump and the integrator was blocked round after round. The plan says
what it changes: a task brief or title naming the constant, or a task's
files naming the header that defines it.
"""
import fnmatch
import posixpath

from .numbers import SOURCES

# kind -> (constant, the plan's "reserved" key)
KINDS = {
    "protocol": ("kProtocolVersion", "protocol"),
    "format": ("kFormatVersion", "formats"),
}


def _names_file(glob, path):
    """True when `glob` names `path` itself, not a whole directory of it."""
    return glob == path or (posixpath.basename(path) in glob
                            and fnmatch.fnmatchcase(path, glob))


def _tasks(plan):
    tasks = plan.get("tasks") if isinstance(plan, dict) else None
    return [t for t in tasks if isinstance(t, dict)] \
        if isinstance(tasks, list) else []


def needed(plan):
    """Kinds (protocol, format) the plan's tasks change."""
    out = []
    for kind, (const, _) in sorted(KINDS.items()):
        path = SOURCES[kind][0]
        for t in _tasks(plan):
            text = f"{t.get('title', '')}\n{t.get('brief', '')}"
            files = [g for g in t.get("files") or () if isinstance(g, str)]
            if const in text or any(_names_file(g, path) for g in files):
                out.append(kind)
                break
    return out


def reserve(store, owner, plan, seed_fn):
    """Reserve every needed kind for `owner` (a feature already holding one
    gets it back) and write the number into a plan that left it empty.
    Raises ForgeError when a seed cannot be read: never guess a number."""
    for kind in needed(plan):
        n = store.reserve(kind, owner, seed_fn(kind))
        res = plan.get("reserved")
        if not isinstance(res, dict):
            continue    # the schema check reports the shape
        key = KINDS[kind][1]
        if key == "formats":
            if isinstance(res.get(key), list) and not res[key]:
                res[key].append(n)
        elif res.get(key) is None:
            res[key] = n

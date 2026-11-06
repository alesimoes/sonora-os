"""Plan gate (owner, 2026-11-06): no implementer for new feature work unless
planning/plans/<id>.md exists with a `Status: approved` line.

The gate is on when .claude/routing.toml has a [planning] table, so a
checkout that predates the process behaves as before. Adopted PRs (the merge
train, fixers, integrators) never come through here: only Flow.ingest_plan,
the one door by which a feature gets tasks, asks.
"""
import os
import re

DEFAULT_DIR = os.path.join("planning", "plans")
DEFAULT_MARKER = "Status: approved"


def enabled(routing):
    return bool(routing and routing.planning())


def plan_file(root, fid, routing=None):
    d = (routing.planning().get("plan_dir") if routing else None) \
        or DEFAULT_DIR
    return os.path.join(root, d, f"{fid}.md")


def approved(root, fid, routing=None):
    """(ok, reason). The marker must be a line of its own (any case, an
    optional leading `-`/`*`/`>`), so prose that quotes it does not count."""
    path = plan_file(root, fid, routing)
    marker = (routing.planning().get("approved_marker") if routing else None) \
        or DEFAULT_MARKER
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return False, f"{path} does not exist"
    rx = re.compile(r"^[ \t>*-]*" + re.escape(marker) + r"[ \t]*$",
                    re.I | re.M)
    if not rx.search(text):
        return False, f"{path} has no line `{marker}`"
    return True, ""


def check(root, fid, routing):
    """[] when the feature may get engineers, else the reasons (also the
    text the architect is sent back with)."""
    if not enabled(routing):
        return []
    ok, why = approved(root, fid, routing)
    if ok:
        return []
    return [f"plan gate: {why}. Write planning/plans/{fid}.md from "
            "planning/plans/TEMPLATE.md and set `Status: approved` once the "
            "contracts and the ADR number are decided"]

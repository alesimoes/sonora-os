"""A PR opened from a slice's own sub-branch carries that slice (2026-11-13).

ampcab.budget's only slice finished before feat/ampcab.budget existed, so
its engineer opened #669 from `sub/ampcab.budget/budget-ir`. The adoption
sweep took it as train entry pr-669, which merged on 2026-11-12, but none of
the links back to a feature knew that shape: feature_pr_merged pairs an
entry with a feature on the feature's own branch, resolver_link with a
branch a stall resolver reported (it skips `sub/<fid>/` on purpose) or a
title that starts with the feature id ("ampcab-budget: ..." does not). The
slice's integrator kept failing pre-pr ("ADR-0152 is also added by open PR
#669"), the principal blocked it, and the feature sat in integrating.

A merged entry whose branch is the sub-branch of a feature's last open
slice merged that slice and so the feature (its branch head is the PR's
head); the feature keeps its `partial` flag, so its rest is continuable
(remainder.py). A slice blocked on an owner decision is left blocked, as
resolver_link does, so the decision stays visible; slices still being
worked on, or with unfinished siblings, are not moved.
"""
from . import blockinfo, resolver_link

DONE = ("merged", "closed")
SETTLED = ("integrating", "blocked")


def _slice_on(store, entry):
    branch = entry.get("branch") or ""
    if not branch.startswith("sub/"):
        return None
    for t in store.tasks(kind="work"):
        if t.get("branch") == branch and t.get("feature_id") \
                and t["feature_id"] != entry["id"] \
                and t["state"] in SETTLED and not blockinfo.kept_open(t):
            return t
    return None


def _running(store, fid):
    ids = {t["id"] for t in store.feature_tasks(fid)}
    return any(r["task_id"] in ids for r in store.active_runs())


def _finished(t):
    return t["state"] in DONE or blockinfo.kept_open(t)


def _only_one_left(store, t):
    """Why only then: the daemon opens a feature's PR when every slice is
    integrating (Orchestrator._ready_features), so one slice moved to merged
    under unfinished siblings would keep their PR from ever opening."""
    return all(_finished(x) for x in store.feature_tasks(t["feature_id"])
               if x["id"] != t["id"])


def carry(orch, t, entry):
    """Slice t, the feature's last open work, merged in entry's PR: so did
    the feature."""
    store, fid, n = orch.store, t["feature_id"], entry["pr"]
    note = f"its work merged in PR #{n} ({entry['id']}) from {t['branch']}"
    with store.tx():
        store.advance(t["id"], "pr_open", note=note)
        store.advance(t["id"], "merged", note=note)
        store.set_feature(fid, state="merged")
    for x in store.feature_tasks(fid):
        orch._release(x)
    store.event(t["id"], "merged_elsewhere", f"PR #{n} as {entry['id']}")


def sweep(orch):
    """Carry the slices whose own PR merged as a train entry. Returns the
    ids of the slices moved."""
    store, moved = orch.store, []
    for e in resolver_link.entries(store):
        if e["state"] != "merged":
            continue
        t = _slice_on(store, e)
        if t is None:
            continue
        f = store.get_feature(t["feature_id"])
        if f is None or f["state"] in DONE or _running(store, f["id"]) \
                or not _only_one_left(store, t):
            continue
        carry(orch, t, e)
        moved.append(t["id"])
    return moved

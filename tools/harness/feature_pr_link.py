"""Record a PR someone else opened on a feature's own branch.

The daemon opens a feature's PR itself once every slice is integrating and
integrated (Orchestrator._ready_features). Until then the adoption sweep
leaves that branch alone (prpending.in_the_making). When a slice is blocked
the feature never becomes ready, so a PR opened by hand or by a stall
resolver on that branch was owned by nobody: ui.editing.commands sat 9 h in
integrating with #651 green and CLEAN on its branch (2026-11-09).

A PR on the branch of a feature whose slices are all finished (integrating)
or blocked, with no agent running on any of them, is that feature's PR: it
is recorded on the feature, which goes to pr_open and is merged by the
feature watch like one the daemon opened. A blocked slice whose branch head
is in the PR moves with it (its work ships there); any other stays blocked.
"""
from .forge import ForgeError

SETTLED = ("integrating", "blocked")
DONE = ("merged", "closed")


def owner_of(store, branch):
    """The feature that would open a PR from `branch` itself and may take
    one opened for it, or None."""
    for f in store.features():
        if f.get("pr") or f.get("branch") != branch:
            continue
        ts = store.feature_tasks(f["id"])
        live = [t for t in ts if t["state"] not in DONE]
        if live and all(t["state"] in SETTLED for t in live) \
                and not _running(store, ts):
            return f
    return None


def _running(store, ts):
    ids = {t["id"] for t in ts}
    return any(r["task_id"] in ids for r in store.active_runs())


def _carried(forge, t, head):
    """True when blocked slice `t`'s branch head is in the PR's head."""
    try:
        tip = forge.branch_tip(t["branch"]) if t.get("branch") else None
        return bool(tip and head and forge.is_ancestor(tip["sha"], head))
    except ForgeError:
        return False


def link(orch, f, n, branch):
    """Make PR #n feature `f`'s PR, as Orchestrator._open_feature_pr does
    after it opens one. Returns the ids of blocked slices moved along."""
    store, fid = orch.store, f["id"]
    ts = store.feature_tasks(fid)
    try:
        tip = orch.forge.branch_tip(branch)
    except ForgeError:
        tip = None
    head = tip["sha"] if tip else None
    moved = []
    carried = [t for t in ts
               if t["state"] == "blocked" and _carried(orch.forge, t, head)]
    with store.tx():
        store.set_feature(fid, pr=n, branch=branch)
        # advance_feature leaves blocked slices where they are, so the
        # carried ones move after it.
        store.advance_feature(fid, "pr_open", pr=n)
        for t in carried:
            store.advance(t["id"], "pr_open",
                          note=f"its work ships in PR #{n}")
            store.set_task(t["id"], pr=n)
            moved.append(t["id"])
    for t in ts:
        orch._release(t)
    orch.ci_polled[fid] = 0
    store.event(ts[0]["id"], "pr_linked", f"#{n} {branch}" + (
        " carries " + ", ".join(moved) if moved else ""))
    if not orch.cfg.daemon_merge:
        orch.forge.enqueue(n)
    return moved

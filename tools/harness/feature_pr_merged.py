"""Close a stuck feature whose branch another entry's PR already merged.

feature_pr_link gives an open PR on a stuck feature's branch to that feature.
A PR adopted by number as a train entry (`pr-N`) before that could happen
merges under the entry, and the store allows a PR number on one feature
only, so the real feature kept pr=None for good: #651 merged as pr-651 and
ui.editing.commands sat in integrating for 11 h with its code on main
(2026-11-09).

Each adoption sweep pairs every stuck feature with a merged entry on the
same branch. The feature is closed only when every finished slice's branch
head is in the merged PR's head; otherwise work that is not on main would
be called merged. A blocked slice in the PR is closed with it, any other
stays blocked.
"""
from . import feature_pr_link
from .forge import ForgeError


def _merged_twins(store):
    """{branch: entry} for the merged features that hold a PR number."""
    return {f["branch"]: f for f in store.features()
            if f.get("pr") and f.get("branch") and f["state"] == "merged"}


def _in_head(forge, t, head):
    try:
        tip = forge.branch_tip(t["branch"]) if t.get("branch") else None
        return bool(tip and head and forge.is_ancestor(tip["sha"], head))
    except ForgeError:
        return False


def _close(orch, f, twin, head):
    store, fid, n = orch.store, f["id"], twin["pr"]
    ts = store.feature_tasks(fid)
    note = f"its work merged in PR #{n} ({twin['id']})"
    along = [t for t in ts
             if t["state"] == "blocked" and _in_head(orch.forge, t, head)]
    with store.tx():
        # advance_feature leaves blocked slices where they are, so the ones
        # in the PR move between its two steps.
        store.advance_feature(fid, "pr_open", note=note)
        for t in along:
            store.advance(t["id"], "pr_open", note=note)
        store.advance_feature(fid, "merged")
    for t in ts:
        orch._release(t)
    # Not "merged": the entry already counted this PR as a merge.
    store.event(ts[0]["id"], "merged_elsewhere", f"PR #{n} as {twin['id']}")


def sweep(orch):
    """Close the stuck features whose branch a merged entry carried.
    Returns their ids."""
    store, closed = orch.store, []
    twins = _merged_twins(store)
    for f in store.features():
        twin = twins.get(f.get("branch"))
        if f.get("pr") or twin is None:
            continue
        owner = feature_pr_link.owner_of(store, f["branch"])
        if owner is None or owner["id"] != f["id"]:
            continue
        pr = orch.forge.pr_detail(twin["pr"])
        head = pr.get("head_sha")
        done = [t for t in store.feature_tasks(f["id"])
                if t["state"] == "integrating"]
        if pr.get("state") != "merged" or pr.get("branch") != f["branch"] \
                or not all(_in_head(orch.forge, t, head) for t in done):
            continue
        _close(orch, f, twin, head)
        closed.append(f["id"])
    return closed

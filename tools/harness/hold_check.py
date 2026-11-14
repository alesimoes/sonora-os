"""A hold that can never be released is an alert (2026-11-13).

Flow holds a feature's tasks (not_before=HOLD) until the feature its plan
names in `depends_on_feature` merges, and releases them only then. When that
feature is closed or was never created, the hold is forever and nothing said
so: ui.comping.exit_test waited on ui.comping.consolidate, closed as "already
on main". The stall check skips held tasks, so this check names them.
"""
from .scheduler import HOLD

NEVER_MERGES = ("closed",)


def _held_tasks(store, fid):
    return [t["id"] for t in store.feature_tasks(fid)
            if t["state"] == "queued" and t["not_before"] >= HOLD]


def _dead_dependency(store, dep):
    """Why the awaited feature will never merge, or None while it may."""
    d = store.get_feature(dep)
    if d is None:
        return "is not in the store"
    if d["state"] in NEVER_MERGES:
        return f"is {d['state']}"
    return None


def stale_holds(store, plan_of):
    """[(feature id, awaited feature, why, held task ids)] for every hold
    whose awaited feature will never merge. plan_of(fid) -> accepted plan."""
    out = []
    for f in store.features():
        if f["state"] in ("merged", "closed"):
            continue
        held = _held_tasks(store, f["id"])
        if not held:
            continue
        dep = (plan_of(f["id"]) or {}).get("depends_on_feature")
        why = _dead_dependency(store, dep) if dep else None
        if why:
            out.append((f["id"], dep, why, held))
    return out


def check_holds(sv, now):
    for fid, dep, why, held in stale_holds(sv.o.store,
                                           sv.o.flow.accepted_plan):
        sv.alert(f"hold:{fid}",
                 f"{', '.join(held)} held until {dep} merges, but {dep} "
                 f"{why}: it will never be released. Re-plan {fid} without "
                 "the dependency or close it.", check="stall")

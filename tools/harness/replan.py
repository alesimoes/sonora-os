"""A re-plan takes effect (2026-11-13).

When the unblock ladder is spent, stall_resolver queues an architect run
(event stall_resolver) to re-plan the feature. Its result was treated like
any other wake: Flow.on_wake_done ingested plans only for `plan_request`,
so the re-plan was never read, no lead was woken, and the old slices stayed
as they were. ui.record.punch_loop was re-planned six times in two days
("merge .ui and .tests into one slice ... lead must close the old tasks")
and sat 83 h in integrating: the brief asked for the markdown plan only, and
the lead has no way to close a task.

Now the daemon applies a re-plan itself:
  * plan file unchanged          -> the re-plan found nothing to change; the
                                    feature's blocked slices wait on the owner
  * plan with no tasks           -> nothing is left to build: feature closed
  * a run still works on it      -> recorded, applied by the tick once idle
  * otherwise                    -> validated like a first plan (overlaps, plan
                                    gate, numbers); the unfinished old slices
                                    close, the next generation of task ids
                                    starts, and the lead distributes it.
"""
from . import closed

EVENT = "stall_resolver"
AGENT = "architect"
GEN_KEY = "plangen:"
PENDING_KEY = "replan_pending:"
KEEP = ("merged", "closed", "pr_open", "ci_fix")
UNCHANGED = ("a re-plan left the plan unchanged and the feature is still "
             "stalled: close it, re-scope it, or say how to unblock {ids}")


def is_replan(t):
    return bool(t.get("event") == EVENT and t.get("agent") == AGENT
                and t.get("feature_id"))


def suffix(store, fid):
    """Task id suffix of the feature's current plan generation."""
    n = int(store.get_meta(GEN_KEY + fid, 0) or 0)
    return f"-r{n}" if n else ""


def _running(store, fid):
    ids = {t["id"] for t in store.feature_tasks(fid)}
    return any(r["task_id"] in ids for r in store.active_runs())


def ingest(flow, t):
    """Flow.on_wake_done for plan_request and re-plan wakes: (ok, errors)."""
    fid = t["feature_id"]
    if not is_replan(t):
        return flow.ingest_plan(fid)
    f = flow.store.get_feature(fid)
    if f is None or f["state"] in ("merged", "closed"):
        return True, []     # carried or closed while the architect ran
    plan, err = flow._read_json(flow.plan_file(fid))
    if err:
        return False, [err + "; a re-plan rewrites this file, not only the "
                       "markdown plan"]
    if plan == flow.accepted_plan(fid):
        _unchanged(flow.o, fid)
        return True, []
    if isinstance(plan, dict) and plan.get("tasks") == []:
        _nothing_left(flow.o, fid)
        return True, []
    if _running(flow.store, fid):
        flow.store.set_meta(PENDING_KEY + fid, "1")
        flow.store.event(t["id"], "replan_pending", f"{fid}: a run is active")
        return True, []
    return apply(flow, fid)


def apply(flow, fid):
    ok, errs = flow.ingest_plan(fid)
    if ok:
        _retire(flow.o, fid)
    return ok, errs


def apply_pending(o):
    """Tick: apply the re-plans that waited for the feature's runs."""
    store = o.store
    for f in store.features():
        if store.get_meta(PENDING_KEY + f["id"]) != "1" \
                or _running(store, f["id"]):
            continue
        store.set_meta(PENDING_KEY + f["id"], "0")
        ok, errs = apply(o.flow, f["id"])
        if not ok:
            store.event(None, "replan_invalid", f"{f['id']}: {errs[:4]}")


def _retire(o, fid):
    """Close the old plan's unfinished slices and start a new generation."""
    store = o.store
    gen = int(store.get_meta(GEN_KEY + fid, 0) or 0) + 1
    gone = []
    with store.tx():
        for x in store.feature_tasks(fid):
            if x["state"] in KEEP:
                continue
            o._release(x)
            store.advance(x["id"], "closed", note=f"replaced by re-plan {gen}")
            gone.append(x["id"])
        store.set_meta(GEN_KEY + fid, gen)
    store.event(None, "replan_applied",
                f"{fid}: generation {gen}; closed {', '.join(gone) or 'none'}")


def _unchanged(o, fid):
    store = o.store
    blocked = [x for x in store.feature_tasks(fid)
               if x["state"] == "blocked" and not x.get("owner")]
    why = UNCHANGED.format(ids=", ".join(x["id"] for x in blocked) or fid)
    for x in blocked:
        store.set_task(x["id"], owner=why)
    store.event(None, "replan_unchanged", f"{fid}: {len(blocked)} to owner")


def _nothing_left(o, fid):
    f = o.store.get_feature(fid)
    if f is not None and f["state"] not in ("merged", "closed"):
        closed.close_feature(o, f, "re-plan: nothing left to build")

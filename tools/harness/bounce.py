"""A tester sends a slice back; the third time it goes to the architect.

Before 2026-11-13 the third bounce blocked the slice as "third bounce: owner
decision" and put it in waits_on_owner. The testers' reasons were claims too
narrow ("live_loops_model.* is outside claimed globs") or a build over the
command cap: nothing the owner could rule on, and the block also kept the
feature away from the stall resolver. Now the slice blocks with cause `plan`
and the architect re-plans the feature with the testers' reasons in its
brief (stall_resolver.queue_replan; replan.py applies the result).
"""
from . import findings, stall_resolver

THIRD = 3


def reasons(data):
    """The tester's summary and blocking findings, one line each."""
    out = [str((data or {}).get("summary") or "").strip()]
    for f in findings.blocking((data or {}).get("findings")):
        where = f.get("where")
        out.append(f"{where}: {f.get('text')}" if where else
                   str(f.get("text")))
    return [x for x in out if x]


def bounce(o, t, data):
    n = t["bounces"] + 1
    o.store.set_task(t["id"], bounces=n)
    if n >= THIRD:
        return third(o, t, data)
    o.store.advance(t["id"], "bugfix")
    o._tell("lead", "qa_fail", {"task": t["id"], "bounces": n},
            feature=t["feature_id"], task=t["id"])
    o._lead_event("qa_fail", t)
    if n == 2:
        o._ensure_wake("architect", "bug_ruling", t["id"],
                       feature=t["feature_id"])


def third(o, t, data):
    why = "; ".join(reasons(data)) or "no reason given"
    o._block(t, f"third bounce from QA, re-plan asked of the architect. "
                f"Tester: {why}"[:1500], cause="plan")
    fid = t.get("feature_id")
    if not fid:
        o._ensure_wake("architect", "bug_ruling", t["id"])
        return
    if stall_resolver.replan_pending(o.store, fid):
        return
    model = "fable" if stall_resolver.is_core(o.store, fid) else "sonnet"
    wid = stall_resolver.queue_replan(o, fid, (
        f"Slice {t['id']} bounced from QA three times. The testers' "
        f"reasons:\n- " + "\n- ".join(reasons(data) or ["(none)"])), model)
    o.store.event(t["id"], "third_bounce", wid)

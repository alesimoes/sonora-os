"""Keep a feature's own state true to what happened to its PR and tasks.

2026-11-08: feature pr-442 stayed pr_open for 68 h after #442 merged, and
mixassist.plans stayed blocked after its only task closed as already on
main; that blocked three features that depend on it. The task-level watch
only looks at features whose first task is still pr_open, so a feature whose
tasks moved on (or ended) was never looked at again.

Each tick, for features the PR watch does not cover:
  * PR merged on GitHub  -> feature merged;
  * PR closed unmerged   -> feature closed;
  * every task merged or closed -> the feature is terminal too (a feature
    left "blocked" or "pr_open" only holds up what depends on it). The same
    holds for a feature still "building", "qa" or "integrating": six
    features whose every task had closed (ui.editing.import,
    ui.shell.fullscreen, main-red-7aeea70f...) still counted as building on
    2026-11-13, because only the PR states were looked at.

The same step closes the features a stall resolver's PR carried
(resolver_link), the slices whose own sub-branch PR merged (slice_pr_link),
and applies the re-plans that waited for a run (replan).
"""
from . import replan, resolver_link, slice_pr_link
from .forge import ForgeError

TERMINAL_TASK = ("merged", "closed")
OPEN_PR = ("pr_open", "ci_fix")
LIVE = OPEN_PR + ("blocked",)
WORKING = ("building", "qa", "integrating")


def _terminal_state(ts):
    """merged when any task merged, closed when all were closed; None while
    a task is still live."""
    states = {t["state"] for t in ts}
    if not states or not states <= set(TERMINAL_TASK):
        return None
    return "merged" if "merged" in states else "closed"


def _covered_by_watch(o, f, ts):
    """True when Orchestrator._watch_pr already polls this feature."""
    if ts[0]["state"] == "pr_open":
        return True
    return ts[0]["state"] == "blocked" and o.train.is_adopted(f["id"])


def _from_tasks(o, f, ts):
    end = _terminal_state(ts)
    if end is None:
        return False
    o.store.set_feature(f["id"], state=end)
    o.store.event(ts[0]["id"], "feature_synced",
                  f"{f['id']}: {f['state']}->{end}, every task is terminal")
    return True


def _from_pr(o, f, ts):
    try:
        st = o.forge.pr_status(f["pr"])
    except ForgeError as e:
        o.store.event(ts[0]["id"], "ci_poll_failed", str(e))
        return
    if st["state"] == "merged":
        # ci_fix may only go back to pr_open; a fixer in flight is moot now.
        for t in ts:
            if t["state"] == "ci_fix":
                o.store.advance(t["id"], "pr_open")
        o._merged(f, ts)
    elif st["state"] == "closed":
        o._close_feature(f, f"PR #{f['pr']} closed without merge")


def sync_one(f, o, now):
    ts = o.store.feature_tasks(f["id"])
    if not ts or f["state"] not in LIVE + WORKING:
        return True
    if _from_tasks(o, f, ts):
        return True
    if not f["pr"] or f["state"] not in OPEN_PR or _covered_by_watch(o, f, ts):
        return True
    if now - o.ci_polled.get(f["id"], 0) < o.cfg.ci_poll_s:
        return True
    o.ci_polled[f["id"]] = now
    _from_pr(o, f, ts)
    return True


def sync(o, now):
    o.guard.step("carried_prs", resolver_link.sweep, o)
    o.guard.step("slice_prs", slice_pr_link.sweep, o)
    o.guard.step("replan_pending", replan.apply_pending, o)
    o.guard.each("feature_sync", o.store.features(), sync_one, o, now,
                 key=lambda f: f["id"], task=o._rep_task)

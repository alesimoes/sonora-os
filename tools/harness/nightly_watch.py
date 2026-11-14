"""The nightly's guarantee (owner, 2026-11-07): a verdict on main every day.

The workflow can be cancelled (a branch dispatch used to), skipped or fail to
start; none of that says anything about main. So the daemon watches the clock
too: no success/failure on main for WATCH_S and nothing queued or running,
it dispatches one (at most once per REDISPATCH_S) and says so in the events.
"""
import calendar
import json
import time

WATCH_S = 26 * 3600.0
REDISPATCH_S = 6 * 3600.0
META_LAST = "nightly-last"
META_DISPATCHED = "nightly-dispatched-at"
DECISIVE = ("success", "failure")
EVENTS = ("schedule", "workflow_dispatch")


def on_main(runs):
    """Drop runs that carry a branch/event other than main's nightly ones
    (the forge already filters; this keeps a fake or a changed forge honest)."""
    return [r for r in runs
            if r.get("branch", "main") == "main"
            and r.get("event", "schedule") in EVENTS]


def _epoch(iso, default):
    try:
        return float(calendar.timegm(time.strptime(iso, "%Y-%m-%dT%H:%M:%SZ")))
    except (TypeError, ValueError):
        return default


def record_last(store, run, now):
    """What the dashboard tile shows: the newest completed run on main."""
    store.set_meta(META_LAST, json.dumps({
        "id": run.get("id"), "sha": str(run.get("head_sha") or "")[:12],
        "conclusion": run.get("conclusion"), "url": run.get("url"),
        "at": _epoch(run.get("updated_at"), now)}))


def status(store):
    """For status.json: {last: {...}|None, dispatched_at, watch_s}."""
    try:
        last = json.loads(store.get_meta(META_LAST) or "null")
    except ValueError:
        last = None
    try:
        disp = float(store.get_meta(META_DISPATCHED) or 0) or None
    except (TypeError, ValueError):
        disp = None
    return {"last": last, "dispatched_at": disp, "watch_s": WATCH_S}


def drop_bogus_tasks(store, done, suffix=""):
    """A main-red task opened for a run that was only cancelled: close it
    while nobody has started it. A failure on the same head keeps it."""
    failed = {str(r.get("head_sha") or "")[:8]
              for r in done if r.get("conclusion") == "failure"}
    for r in done:
        if r.get("conclusion") in DECISIVE:
            continue
        sha8 = str(r.get("head_sha") or "")[:8]
        t = store.get_task(f"main-red-{sha8}{suffix}") if sha8 else None
        if t and t["state"] == "queued" and sha8 not in failed:
            store.advance(t["id"], "closed",
                          f"nightly run {r.get('id')} was {r.get('conclusion')}"
                          ", not a failure")


def watchdog(orch, runs, now, workflow):
    """Dispatch a nightly on main when the last verdict is older than
    WATCH_S, none is in flight, and none was dispatched in REDISPATCH_S."""
    store = orch.store
    if any(r.get("status") != "completed" for r in runs):
        return None
    verdicts = [r for r in runs if r.get("conclusion") in DECISIVE]
    if verdicts:
        at = _epoch(verdicts[0].get("updated_at"), now)
        if now - at < WATCH_S:
            return None
    try:
        sent = float(store.get_meta(META_DISPATCHED) or 0)
    except (TypeError, ValueError):
        sent = 0.0
    if now - sent < REDISPATCH_S:
        return None
    store.set_meta(META_DISPATCHED, str(now))
    try:
        orch.forge.dispatch_workflow(workflow, "main")
    except Exception as e:
        store.event("nightly", "dispatch_failed", str(e)[:200])
        return None
    store.event("nightly", "nightly_dispatched",
                "no verdict on main in the last 26 h")
    return True

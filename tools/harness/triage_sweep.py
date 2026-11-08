"""Triage of tasks that were ALREADY blocked (owner 2026-11-08, after #557).

`triage.handle` only runs when an agent reports `blocked`, so a block made
before the daemon loaded the rule (or by a path that never reaches it) sat
there for ever. On startup and then every hour this sweep reads the note of
each blocked task that does not wait on the owner and applies the same
classes, so no human has to look.

A janitor wake blocked by "containers running" is closed: the next merge
wakes the janitor again, a retry of the old one would only pile up."""
from . import blockinfo, triage

SWEEP_EVERY_S = 3600.0
ROLE = "engineer"


def _is_busy_janitor_wake(t):
    return (t["kind"] == "wake" and t["agent"] == "janitor"
            and blockinfo.is_retry_later("janitor", t.get("note")))


def close_wake(o, t, why):
    """The wake is done for: the next trigger makes a fresh one."""
    o._release(t)
    o.store.advance(t["id"], "closed", note=why)
    o.store.event(t["id"], "triage_wake_closed", why[:200])


def _worth_trying(o, t, why):
    """Skip what handle() would refuse again, so an hourly sweep does not
    churn events on a task that really is stuck."""
    cls = triage.classify(ROLE, why)
    if cls is None:
        return False
    if cls == "nocommit":
        return (triage.count(t.get("note"), "nocommit") < 1
                or triage.on_main(o, t, {}))
    if cls == "scope":
        return triage.count(t.get("note"), "scope") < 3
    return True


def _revive(o, t, why, now):
    """Put the task back to a live state, let triage act; re-block it when
    triage declines."""
    cause = t.get("cause")
    o.store.advance(t["id"], "building", note=why)
    fresh = o.store.get_task(t["id"])
    if triage.handle(o, fresh, ROLE, why, {}, now):
        return True
    o.store.advance(t["id"], "blocked", note=why)
    o.store.set_task(t["id"], cause=cause)
    return False


def sweep_one(o, t, features, now):
    why = t.get("note") or ""
    if t.get("owner") or blockinfo.superseded(t, features):
        return False
    if _is_busy_janitor_wake(t):
        close_wake(o, t, "janitor: containers running; next merge wakes it")
        return True
    if t["kind"] != "work" or not _worth_trying(o, t, why):
        return False
    return _revive(o, t, why, now)


def sweep(o, now):
    """Returns the number of tasks triage dealt with."""
    features = {f["id"]: f for f in o.store.features()}
    n = 0
    for t in o.store.tasks():
        if t["state"] != "blocked":
            continue
        if o.guard.step("triage_sweep_one", sweep_one, o, t, features, now,
                        key=t["id"], task=t["id"]):
            n += 1
    if n:
        o.log(f"triage sweep dealt with {n} blocked task(s)")
    return n


def tick(o, now):
    """Startup (first tick of this process) and then once per hour."""
    last = getattr(o, "triage_swept_at", None)
    if last is not None and now - last < SWEEP_EVERY_S:
        return 0
    o.triage_swept_at = now
    return sweep(o, now)

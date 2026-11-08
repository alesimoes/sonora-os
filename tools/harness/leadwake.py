"""When the lead may be woken for a track under its floor (cost bug 2026-11-08).

scheduler.plan() reports pool_under_floor every tick a track has no eligible
task. While the lead's plans were rejected that looped every 30 s and burned
38 of 60 hourly launches. Two guards, both pure:

- no wake while the track has a queued, planning or distributing feature:
  work is already coming;
- otherwise at most one wake per track per cfg.lead_wake_cooldown_s.
"""

BUSY_FEATURE_STATES = ("queued", "planning", "distributing")
KIND = "pool_under_floor"


def _busy(features, track):
    return any(f["track"] == track and f["state"] in BUSY_FEATURE_STATES
               for f in features)


def last_wake(wake_tasks, track):
    """Creation time of the newest lead wake for the track, or None."""
    prefix = f"wake-lead-{KIND}-{track}-"
    times = [w["created"] or 0 for w in wake_tasks
             if w["id"].startswith(prefix)]
    return max(times) if times else None


def allowed(wakes, features, wake_tasks, now, cooldown_s):
    """Filter plan.wakes; other wake kinds pass untouched."""
    out = []
    for wake in wakes:
        role, kind, track = wake
        if role == "lead" and kind == KIND:
            if _busy(features, track):
                continue
            last = last_wake(wake_tasks, track)
            if last is not None and now - last < cooldown_s:
                continue
        out.append(wake)
    return out


def count_last_hour(wake_tasks, now):
    """Lead wakes created in the last hour, for the dashboard."""
    return sum(1 for w in wake_tasks if w["id"].startswith("wake-lead-")
               and now - (w["created"] or 0) < 3600)

"""A feature blocked because its screenshots failed is tried again
(2026-11-13).

The daemon regenerates a UI feature's pictures before it opens the PR
(shots.py); after MAX_TRIES failures it blocks the whole feature, and
nothing ever looked at it again: the notes are a build log that matches no
triage class and a blocked feature raises no stall alert. automation.write
sat blocked that way with every slice integrated and its branch merging
cleanly ("no code blocker ... nothing opened the PR", stall resolver 662):
the pictures had failed while Docker was down.

The hourly triage sweep puts such a feature back to integrating, with the
picture job's tries reset, while Docker is up, at most MAX_REVIVES times;
the PR then opens as usual. Past that it stays blocked for a person.
"""
PREFIX = "screenshot regeneration failed"
MAX_REVIVES = 3
KEY = "shots_revive:"


def _docker_down(o, now):
    guard = getattr(o, "docker", None)
    if guard is None:
        return False
    try:
        return bool(guard.held(now))
    except Exception:  # noqa: BLE001 - unknown: do not spend a try
        return True


def revive(o, t, now):
    """True when t's feature went back to integrating."""
    t = o.store.get_task(t["id"]) or t
    if t["kind"] != "work" or t["state"] != "blocked" \
            or not (t.get("note") or "").startswith(PREFIX):
        return False
    fid = t["feature_id"]
    f = o.store.get_feature(fid)
    n = int(o.store.get_meta(KEY + fid, 0) or 0)
    if f is None or f["state"] != "blocked" or n >= MAX_REVIVES:
        return False
    blocked = [x for x in o.store.feature_tasks(fid)
               if x["state"] == "blocked"]
    if not all(x.get("integrated") for x in blocked) \
            or _docker_down(o, now):
        return False
    note = f"pictures tried again ({n + 1}/{MAX_REVIVES})"
    with o.store.tx():
        for x in blocked:
            o.store.advance(x["id"], "integrating", note=note)
        o.store.set_meta(KEY + fid, n + 1)
        o.store.set_meta(f"shots:{fid}", "{}")
        o.store.set_feature(fid, state="integrating")
    o.store.event(t["id"], "shots_revived", note)
    return True

"""A PR closed without merge is an owner decision already taken (superseded,
carried by another PR), not a block waiting for one. Its tasks end in the
terminal state "closed", next to "merged": the train slot and the engineer
names are freed and nothing alerts on it."""

CLOSED_WHY = "closed without merge"


def close_feature(o, f, why=CLOSED_WHY):
    """Move every live task of the feature (blocked ones too) to closed."""
    for x in o._feature_tasks(f["id"]):
        if x["state"] in ("merged", "closed"):
            continue
        o._release(x)
        o.store.advance(x["id"], "closed", note=why)
    o.store.set_feature(f["id"], state="closed")
    ts = o.store.feature_tasks(f["id"])
    if ts:
        o.store.event(ts[0]["id"], "closed", why)


def migrate(o):
    """On daemon start: tasks an older daemon blocked with 'closed without
    merge' become closed. Real blocks are left alone. Returns the count."""
    n = 0
    for f in o.store.features():
        ts = o._feature_tasks(f["id"])
        if f["state"] != "blocked" or not ts:
            continue
        notes = [t.get("note") or "" for t in ts if t["state"] == "blocked"]
        if any(CLOSED_WHY in x for x in notes):
            close_feature(o, f, notes[0] if notes else CLOSED_WHY)
            n += 1
    return n

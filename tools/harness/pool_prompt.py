"""What the lead reads when a track is under its floor (pool_under_floor).

2026-11-09: progress.sh still listed merged items as open, every proposal of
a wake was a feature already on main (time.tempo_edit, time.groove, rf64...),
each was rejected, and the DAW floor of 8 sat at 0 for hours. So the prompt
lists every feature id the daemon has.

2026-11-13: that list called every merged id done, the 45 features merged
`[partial]` included, and the pools idled with work left. Partial features
whose remainder may be opened are now listed apart, with the PRs that
merged the earlier parts (remainder.py).
"""
from . import queueimport, remainder

WAKE_PREFIX = "wake-lead-pool_under_floor-"
MAX_ROWS = 15


def track_of(wake_id):
    """The track a pool_under_floor wake was queued for, or None."""
    if not (wake_id or "").startswith(WAKE_PREFIX):
        return None
    return wake_id[len(WAKE_PREFIX):].rsplit("-", 1)[0] or None


def known(store):
    by = {}
    for f in store.features():
        by.setdefault(f["state"], []).append(f["id"])
    rows = [f"{st}: {', '.join(sorted(ids))}" for st, ids in sorted(by.items())]
    return ("Features the daemon already has; proposing one of these is "
            "rejected, so pick others (a merged one is done even if "
            "progress.sh still lists it open), except the partial ones "
            "listed as continuable below:\n" + "\n".join(rows))


def continuable(store, track=None):
    rows = remainder.continuable(store, track)
    if not rows:
        return ""
    lines = [f"- {r['root']} ({r['track']}, merged as "
             f"{', '.join('#%d' % p for p in r['prs']) or 'PR unknown'}): "
             f"propose feature {r['next']}" for r in rows[:MAX_ROWS]]
    more = len(rows) - MAX_ROWS
    if more > 0:
        lines.append(f"- ... and {more} more")
    return ("Partial features you may continue (their remainder is planned "
            "from what the merged PR left open; same backlog_ids as the "
            "root):\n" + "\n".join(lines))


def spent(store, track=None):
    ids = remainder.spent(store, track)
    if not ids:
        return ""
    return (f"Partial after {remainder.MAX_ROUNDS} remainders, not offered "
            "again (the owner decides): " + ", ".join(sorted(ids)))


def out_of_scope():
    return ("Out of v1, never propose (rejected): "
            + queueimport.OUT_OF_V1_TEXT + ".")


def parked(store, track=None):
    ids = remainder.parked(store, track)
    if not ids:
        return ""
    return ("Parked for a hardware run (owner 2026-11-13: hardware-only "
            "validations wait until the product is complete; do not "
            "propose them or their remainder): " + ", ".join(sorted(ids)))


def text(store, wake_id=None):
    track = track_of(wake_id)
    parts = [known(store), continuable(store, track), spent(store, track),
             parked(store, track), out_of_scope()]
    return "\n\n".join(p for p in parts if p)

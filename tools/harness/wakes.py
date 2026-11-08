"""When a wake for the same event is already pending (ADR-0047 4a).

Only a wake still waiting to be served suppresses a new one. A blocked wake
is a failed delivery: counting it as pending made one early block (a lead
wake with no recipient, 2026-11-07) swallow every later wake for its track
and left the pools empty for hours."""

SUPPRESSING_STATES = ("queued",)


def is_pending(wakes, prefix):
    return any(w["id"].startswith(prefix) and w["state"] in SUPPRESSING_STATES
               for w in wakes)

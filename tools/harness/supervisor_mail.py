"""Supervisor check for mail the hub could not deliver.

2026-11-07: three leads sent a plan_request to 'orchestrator'; the hub knew
only 'daemon', answered no_recipient, and the pools sat under their floors
with nothing on the board. The broker now emits `undeliverable` for such a
send (protocol.CRITICAL_TYPES) and the orchestrator remembers it; this check
keeps an alert up while the failure is recent.
"""

ALERT_TTL_S = 3600.0


def check_undeliverable(sv, now):
    rows = getattr(sv.o, "undeliverable", {})
    for key in [k for k, (ts, _) in rows.items() if now - ts > ALERT_TTL_S]:
        del rows[key]
    for (sender, to, type_), (_, text) in rows.items():
        sv.alert(f"undeliverable:{sender}:{to}:{type_}",
                 f"undeliverable {text}: the request is lost until the "
                 "sender retries to a name the hub knows", check="mail")

"""One console run kept alive between owner messages (owner, 2026-11-08).

Starting a run costs ~20 s before the model reads anything, so the first
message launches it and the next ones are handed to the same run through the
hub (`owner_ask`, body {"req", "text"}). The run answers each as it arrives
and waits for the next. The daemon recycles it after MAX_MESSAGES (context
grows) or IDLE_S without a message by sending `owner_ask` {"end": true}; the
run finishes any open reply, writes result.json and exits, and the next
message launches a fresh one.

State lives in the store's meta so a daemon restart keeps counting.
"""
from . import console_store as S

NAME = "console"
MAX_MESSAGES = 20
IDLE_S = 900.0
TYPE = "owner_ask"


def _meta(orch, key, default="0"):
    return orch.store.get_meta(key, default) or default


def is_active(orch):
    return any(r["name"] == NAME for r in orch.store.active_runs())


def start(orch, pending, now):
    """A new run begins: its inbox already lists the pending messages."""
    for m in pending:
        orch.store.set_meta("console_fed:" + m["id"], "1")
    orch.store.set_meta("console_sess_n", str(len(pending)))
    orch.store.set_meta("console_sess_last", str(now))
    orch.store.set_meta("console_sess_end", "0")


def _end_requested(orch):
    return _meta(orch, "console_sess_end") == "1"


def _request_end(orch, why):
    orch.store.set_meta("console_sess_end", "1")
    orch._tell(NAME, TYPE, {"end": True, "why": why})
    orch.store.event(None, "console_session_end", why)


def feed(orch, pending, now):
    """Hands unseen messages to the live run; ends it when worn out or idle."""
    if _end_requested(orch):
        return
    n = int(_meta(orch, "console_sess_n"))
    last = float(_meta(orch, "console_sess_last", str(now)))
    if n >= MAX_MESSAGES:
        return _request_end(orch, f"{n} messages")
    fresh = [m for m in pending if _meta(orch, "console_fed:" + m["id"], "") != "1"]
    for m in fresh:
        orch._tell(NAME, TYPE, {"req": m["id"], "text": str(m.get("text") or "")[:S.MAX_TEXT]})
        orch.store.set_meta("console_fed:" + m["id"], "1")
    if fresh:
        orch.store.set_meta("console_sess_n", str(n + len(fresh)))
        orch.store.set_meta("console_sess_last", str(now))
    elif not pending and now - last > IDLE_S:
        _request_end(orch, "idle")

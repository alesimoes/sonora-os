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


# 2026-11-13: console runs answered, ended their turn without result.json
# ("no owner_ask asked for the end") and exited normally. The daemon called
# each one dead, counted a failure and relaunched the console for a message
# already answered. A console session's result.json carries nothing, so a
# clean end of the CLI (its final `result` line says success) is its end.
_TAIL_BYTES = 8192


def _ended_ok(stream_path):
    try:
        with open(stream_path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - _TAIL_BYTES))
            lines = f.read().decode("utf-8", "replace").splitlines()
    except (OSError, TypeError):
        return False
    for ln in reversed(lines):
        if '"type":"result"' in ln.replace(" ", ""):
            flat = ln.replace(" ", "")
            return '"subtype":"success"' in flat and '"is_error":true' not in flat
    return False


def clean_exit(run, alive):
    """A `done` result for a console run whose process ended cleanly with
    no result.json; None for anything else (still dead when it crashed)."""
    if alive or run.get("name") != NAME or not _ended_ok(run.get("stream_path")):
        return None
    return {"role": run["role"], "task": run["task_id"], "status": "done",
            "summary": "console session ended (turn over, no result.json)"}

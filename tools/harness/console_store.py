"""Files of the owner's console (owner, 2026-11-08), under .harness/console/.

The daemon is the only writer; the dashboard only reads. Three files:
- chat.jsonl    append-only, one message per line (the conversation);
- actions.json  {action_id: record}, rewritten whole and atomically (the
                cards the owner has to confirm, and what became of them);
- audit.jsonl   append-only, every action decision (proposed, confirmed,
                cancelled, executed, failed, expired, refused).

Time is ISO text everywhere, so messages from this file and from the
broker's log sort together.
"""
import json
import os
import time

from . import protocol as P
from .mailbox import atomic_write

DIR = "console"
HISTORY = 200
MAX_TEXT = 4000
PENDING_TTL_S = 900.0     # a card nobody answers in 15 min expires
LOG_TAIL_BYTES = 400000
KINDS = ("user", "orch", "agent", "alert", "system")


def console_dir(hdir):
    return os.path.join(hdir, DIR)


def _path(hdir, name):
    return os.path.join(console_dir(hdir), name)


_last_ns = 0


def new_id(prefix):
    """Strictly increasing within the process: Windows' clock ticks coarsely,
    and an action's EXECUTADO line once got the same id as the owner's
    message it answered, which the console then drew twice (2026-11-09)."""
    global _last_ns
    _last_ns = max(time.time_ns(), _last_ns + 1)
    return f"{prefix}{_last_ns:020d}"


def _append(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, sort_keys=True) + "\n")


def _tail(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            start = max(0, f.tell() - LOG_TAIL_BYTES)
            f.seek(start)
            lines = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []
    out = []
    for ln in lines[1:] if start else lines:
        try:
            row = json.loads(ln)
        except ValueError:
            continue
        if isinstance(row, dict):
            out.append(row)
    return out


# -- chat -------------------------------------------------------------
def add_message(hdir, kind, text="", **extra):
    """Appends one message and returns it. kind is one of KINDS."""
    if kind not in KINDS:
        raise ValueError(f"bad message kind {kind}")
    msg = {"id": new_id("m"), "ts": P.now_iso(), "kind": kind,
           "text": str(text)[:MAX_TEXT]}
    msg.update({k: v for k, v in extra.items() if v is not None})
    _append(_path(hdir, "chat.jsonl"), msg)
    return msg


def history(hdir, limit=HISTORY):
    return _tail(_path(hdir, "chat.jsonl"))[-limit:]


def unanswered(hdir):
    """Owner messages for the orchestrator that no orch message answers."""
    rows = history(hdir)
    answered = {m.get("req") for m in rows if m.get("kind") in ("orch", "system")}
    return [m for m in rows if m["kind"] == "user" and not m.get("to")
            and m["id"] not in answered]


# -- partial answer ---------------------------------------------------
def set_partial(hdir, req, text):
    """Text the console run sent before its final reply; one at a time."""
    atomic_write(_path(hdir, "partial.json"),
                 json.dumps({"req": req, "text": str(text)[:MAX_TEXT], "ts": P.now_iso()}).encode())


def get_partial(hdir):
    try:
        with open(_path(hdir, "partial.json"), encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        return None
    return d if isinstance(d, dict) and d.get("text") else None


def clear_partial(hdir):
    try:
        os.remove(_path(hdir, "partial.json"))
    except OSError:
        pass


# -- actions ----------------------------------------------------------
def load_actions(hdir):
    try:
        with open(_path(hdir, "actions.json"), encoding="utf-8") as f:
            d = json.load(f)
    except (OSError, ValueError):
        return {}
    return d if isinstance(d, dict) else {}


def save_actions(hdir, actions):
    keep = sorted(actions.values(), key=lambda a: a["created_ts"])[-HISTORY:]
    atomic_write(_path(hdir, "actions.json"),
                 json.dumps({a["id"]: a for a in keep}).encode())


def audit(hdir, event, action_id, **detail):
    _append(_path(hdir, "audit.jsonl"),
            {"ts": P.now_iso(), "event": event, "action": action_id, **detail})


def audit_tail(hdir, limit=50):
    return _tail(_path(hdir, "audit.jsonl"))[-limit:]


def effective_state(action, now):
    """The state to show: a pending card past its time is expired even
    before the daemon's sweep writes it."""
    if action["state"] == "pending" and now - action["created_ts"] > PENDING_TTL_S:
        return "expired"
    return action["state"]

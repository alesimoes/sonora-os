"""The dashboard's side of the console door (owner, 2026-11-08).

Same door as harness/ownerchat.py: the dashboard has no broker token and
never writes the daemon's files, so it drops a request under
.harness/owner-requests/ and the daemon's next tick consumes it
(console.handle_request). Kinds:
  console  {text, to?}     an owner message; `to` names an agent (then it is
                           an ownerchat ask) or is empty (the orchestrator);
  confirm  {action_id}     run the card; cancel / retry likewise.
Nothing here runs an action: only `confirm`, on the daemon, does.
"""
import json
import os
import time

from . import ownerchat
from . import protocol as P
from .mailbox import atomic_write

KINDS = ("console", "confirm", "cancel", "retry")
MAX_TEXT = 2000


def post(hdir, kind, **fields):
    """Queues one request; returns its file. ValueError when malformed."""
    if kind not in KINDS:
        raise ValueError("unknown request kind")
    req = {"kind": kind}
    if kind == "console":
        text = str(fields.get("text") or "").strip()
        to = fields.get("to") or None
        if not text:
            raise ValueError("empty message")
        if to is not None and not P.valid_name(str(to)):
            raise ValueError("bad agent name")
        req.update(text=text[:MAX_TEXT], to=to)
    else:
        aid = str(fields.get("action_id") or "")
        if not (aid.startswith("a") and aid[1:].isdigit()):
            raise ValueError("bad action id")
        req["action_id"] = aid
    path = os.path.join(ownerchat.request_dir(hdir),
                        f"{time.time_ns()}-{os.getpid()}.json")
    atomic_write(path, json.dumps(req).encode())
    return path

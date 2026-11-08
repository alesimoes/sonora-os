"""The console's endpoints (docs/design/dashboard-console/SPEC.md, section 8).

    GET  /api/console                   strip, agents, attention, chat
    POST /api/chat    {text, to?}       an owner message
    POST /api/action/<id>/confirm       run the card (the only way to run one)
    POST /api/action/<id>/cancel | retry

Nothing here changes anything: each POST drops a request at the daemon's door
(harness/console_door.py) and the daemon decides and acts on its tick. A
message or decision is refused while the daemon is down, because nobody would
consume it."""
import os

import dashboard_console_stream as stream
from dashboard_console import build
from dashboard_daemon import daemon_alive
from dashboard_state import lock, state
from dashboard_util import orch_root
from harness import console_chat, console_door
from harness.console_store import MAX_TEXT

DOWN = "o daemon não está rodando: o pedido ficaria parado. Reinicie o daemon."


def harness_dir():
    return os.path.join(orch_root(), ".harness")


def view():
    with lock:
        snap = dict(state)
    alive = daemon_alive()
    out = build(snap, alive)
    out["chat"] = console_chat.view(harness_dir())
    return out


def stream_signature():
    """Changes whenever view() would: the chat files or the daemon snapshot."""
    with lock:
        updated = state.get("updated")
    return (stream.file_signature(harness_dir()), updated, daemon_alive())


def frames(**kw):
    return stream.frames(view, stream_signature, **kw)


def _post(kind, **fields):
    if not daemon_alive():
        return 503, {"error": DOWN}
    try:
        console_door.post(harness_dir(), kind, **fields)
    except ValueError as e:
        return 400, {"error": str(e)}
    return 202, {"queued": kind}


def handle_post(path, payload):
    """(status, json body) for the console's POST routes."""
    if not isinstance(payload, dict):
        return 400, {"error": "expected a JSON object"}
    if path == "/api/chat":
        return _post("console", text=str(payload.get("text") or "")[:MAX_TEXT],
                     to=payload.get("to") or None)
    parts = path.strip("/").split("/")
    if len(parts) == 4 and parts[:2] == ["api", "action"] and parts[3] in ("confirm", "cancel", "retry"):
        return _post(parts[3], action_id=parts[2])
    return 404, {"error": "unknown endpoint"}

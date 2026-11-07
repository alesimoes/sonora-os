"""The owner talks to an agent from the dashboard: the POST endpoints and the
conversation view of one run. The daemon does the sending
(harness/ownerchat.py); this side only queues requests and reads the log.

Only runs the orchestrator started are known. A subagent started from a
coordinator session with the Agent tool, or a plain terminal session, is not
registered with the broker and cannot be reached; the panel says so.
"""
import os
import time

from dashboard_orch import orch_state
from dashboard_util import orch_root
from harness import ownerchat

FRESH_S = 120.0     # status.json younger than this: a daemon is running
NOT_REACHABLE = ("Sessão fora do orquestrador (subagente da ferramenta Agent ou "
                 "terminal): não está no broker, não dá para falar com ela daqui.")


def harness_dir():
    return os.path.join(orch_root(), ".harness")


def daemon_alive(now=None):
    path = os.path.join(orch_root(), "planning", "orchestrator", "status.json")
    try:
        return (now or time.time()) - os.path.getmtime(path) < FRESH_S
    except OSError:
        return False


def known_run(name):
    runs = [r for r in orch_state().get("runs", []) if r.get("name") == name]
    return runs[-1] if runs else None


def chat_view(run):
    """Added to the run's drawer: the thread, queued requests, what is possible."""
    name = run.get("name") or ""
    hdir = harness_dir()
    live = daemon_alive()
    return {"name": name, "daemon_alive": live,
            "running": not run.get("ended"),
            "thread": ownerchat.thread(hdir, name),
            "pending": ownerchat.pending(hdir, name)}


def handle_post(path, payload):
    """(status, json body) for POST /api/agent/ask and /api/agent/wake."""
    kind = path.rsplit("/", 1)[-1]
    if kind not in ("ask", "wake") or path != "/api/agent/" + kind:
        return 404, {"error": "unknown endpoint"}
    if not isinstance(payload, dict):
        return 400, {"error": "expected a JSON object"}
    name = str(payload.get("name") or "")
    if known_run(name) is None:
        return 404, {"error": "agente desconhecido do orquestrador: " + NOT_REACHABLE}
    if not daemon_alive():
        return 503, {"error": "o daemon não está rodando; o pedido não seria atendido"}
    try:
        ownerchat.write_request(harness_dir(), kind, name, str(payload.get("text") or ""))
    except ValueError as e:
        return 400, {"error": str(e)}
    return 202, {"queued": kind, "name": name}

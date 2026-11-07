"""The owner talks to an agent from the dashboard (owner, 2026-11-06).

The dashboard has no broker token and must not write to the daemon's files,
so it drops a request under .harness/owner-requests/ and the daemon's next
tick consumes it (same door as autoadopt's adopt-requests, one writer for
state.json and the broker):
- ask:  the daemon sends the agent an `owner_ask` message {text}; the agent
        answers `to=daemon type=answer reply_to=<id>` the next time it calls
        `recv` (mail arrives between tool calls only);
- wake: an agent with no live run is relaunched at once (its backoff is
        skipped, attempts are not reset); one that is running is left alone.
The dashboard shows the conversation from log/messages.jsonl.
"""
import json
import os
import time

from . import protocol as P
from .mailbox import atomic_write

REQUEST_DIR = "owner-requests"
MAX_TEXT = 2000
THREAD_LIMIT = 30
LOG_TAIL_BYTES = 1500000
REPLY_HINT = ("Answer briefly with send(to='daemon', type='answer', "
              "reply_to=<this message id>): what you are doing, what is "
              "next, anything blocking. Then carry on.")


def request_dir(hdir):
    return os.path.join(hdir, REQUEST_DIR)


def write_request(hdir, kind, to, text=""):
    """Queues a request. Returns the file, or raises ValueError."""
    if kind not in ("ask", "wake"):
        raise ValueError("kind must be ask or wake")
    if not P.valid_name(to):
        raise ValueError("bad agent name")
    text = (text or "").strip()
    if kind == "ask" and not text:
        raise ValueError("empty message")
    path = os.path.join(request_dir(hdir),
                        f"{time.time_ns()}-{os.getpid()}.json")
    atomic_write(path, json.dumps(
        {"kind": kind, "to": to, "text": text[:MAX_TEXT]}).encode())
    return path


def pending(hdir, to):
    """Requests for `to` the daemon has not consumed yet."""
    n = 0
    for r in _read_all(hdir, remove=False):
        n += r.get("to") == to
    return n


def _read_all(hdir, remove):
    d = request_dir(hdir)
    try:
        names = sorted(f for f in os.listdir(d) if f.endswith(".json"))
    except OSError:
        return []
    out = []
    for n in names:
        path = os.path.join(d, n)
        try:
            with open(path, encoding="utf-8") as f:
                req = json.load(f)
            if remove:
                os.remove(path)
        except (OSError, ValueError):
            continue    # half-written: the next tick sees it whole
        if isinstance(req, dict):
            out.append(req)
    return out


# -- daemon side ---------------------------------------------------------
def tick(orch):
    for req in _read_all(orch.hdir, remove=True):
        name = req.get("to")
        run = orch.store.last_run_of_name(name) if P.valid_name(name) else None
        if run is None:
            orch.store.event(None, "owner_request_rejected",
                             f"{req.get('kind')} {name}: no such agent")
        elif req.get("kind") == "ask":
            ask(orch, run, req.get("text") or "")
        elif req.get("kind") == "wake":
            wake(orch, run)


def ask(orch, run, text):
    task = orch.store.get_task(run["task_id"]) or {}
    orch._tell(run["name"], "owner_ask",
               {"text": text[:MAX_TEXT], "reply": REPLY_HINT},
               feature=task.get("feature_id"), task=run["task_id"])
    orch.store.event(run["task_id"], "owner_ask", f"{run['name']}: {text[:120]}")


def wake(orch, run):
    """Relaunches an agent whose run ended, now instead of after the backoff."""
    tid = run["task_id"]
    if any(r["name"] == run["name"] for r in orch.store.active_runs()):
        orch.store.event(tid, "owner_wake", f"{run['name']}: already running")
        return "running"
    t = orch.store.get_task(tid) or {}
    if t.get("state") in ("blocked", "merged", "closed", None):
        orch.store.event(tid, "owner_wake", f"{run['name']}: task is "
                         f"{t.get('state')}, not relaunched")
        return "not_relaunchable"
    orch.store.set_task(tid, not_before=orch.clock())
    orch.store.event(tid, "owner_wake", f"{run['name']}: relaunch now")
    return "relaunch"


# -- dashboard side ------------------------------------------------------
def _tail_lines(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            start = max(0, f.tell() - LOG_TAIL_BYTES)
            f.seek(start)
            lines = f.read().decode("utf-8", "replace").splitlines()
            return lines[1:] if start else lines   # the first may be cut
    except OSError:
        return []


def _text_of(body):
    if isinstance(body, dict):
        return str(body.get("text") or body.get("summary")
                   or json.dumps(body, sort_keys=True))
    return str(body)


def thread(hdir, name, limit=THREAD_LIMIT):
    """What the owner and the agent said, oldest first: owner_ask to the
    agent and everything the agent sent to the daemon."""
    out = []
    for ln in _tail_lines(os.path.join(hdir, "log", "messages.jsonl")):
        try:
            m = json.loads(ln)
        except ValueError:
            continue
        mine = m.get("to") == name and m.get("type") == "owner_ask"
        theirs = m.get("from") == name and m.get("to") == P.DAEMON
        if mine or theirs:
            out.append({"id": m.get("id"), "ts": m.get("ts"),
                        "who": "owner" if mine else name,
                        "type": m.get("type"), "reply_to": m.get("reply_to"),
                        "text": _text_of(m.get("body"))[:MAX_TEXT]})
    return out[-limit:]

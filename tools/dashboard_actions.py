"""The registry and runner of the actions the dashboard can run (owner,
2026-11-07). An allowlist: an action is registered with a validator and a
runner; nothing else can be asked for, and there is no free-form shell.

submit() validates the parameters against what exists, refuses a destructive
action that was not confirmed, and runs it in a background thread; the
runner gets the action's timeout and passes it to every command it starts.
The output is read with job_view(); every finished job goes to the audit
log (dashboard_audit)."""
import threading
import time
import uuid
from collections import namedtuple

import dashboard_audit

Action = namedtuple("Action", "name area label destructive timeout validate run")
REGISTRY = {}
JOBS = {}
_lock = threading.Lock()
MAX_JOBS = 60


class ActionError(ValueError):
    """The request is refused; the message says why, in plain words."""


def register(name, area, label, validate, run, destructive=False, timeout=60):
    REGISTRY[name] = Action(name, area, label, destructive, timeout, validate, run)


def catalogue():
    return [{"name": a.name, "area": a.area, "label": a.label, "destructive": a.destructive}
            for a in REGISTRY.values()]


def _key(name, params):
    return name + repr(sorted(params.items()))


def _finish(job, ok, output):
    job.update(state="done" if ok else "failed", ok=ok, output=str(output)[-4000:], ended=time.time())
    dashboard_audit.record(job["action"], job["params"], job["state"], job["output"], ok)


def _worker(job, act):
    try:
        ok, output = act.run(job["params"], act.timeout)
    except Exception as e:      # an action never takes the server down
        ok, output = False, "%s: %s" % (type(e).__name__, e)
    _finish(job, bool(ok), output)


def _trim():
    ended = sorted((j for j in JOBS.values() if j["ended"]), key=lambda j: j["ended"])
    for old in ended[:max(0, len(JOBS) - MAX_JOBS)]:
        JOBS.pop(old["id"], None)


def submit(name, params, confirm=False, wait=False):
    """(http status, body). 202 with the job id when it was started; with
    wait=True (tests) the call returns when the job ends."""
    act = REGISTRY.get(name)
    if act is None:
        return 404, {"error": "ação desconhecida: %s" % name}
    if not isinstance(params, dict):
        return 400, {"error": "params must be an object"}
    try:
        clean = act.validate(dict(params))
    except ActionError as e:
        return 400, {"error": str(e)}
    if act.destructive and confirm is not True:
        return 409, {"error": "ação destrutiva: precisa de confirmação", "needs_confirm": True}
    with _lock:
        key = _key(name, clean)
        if any(j["key"] == key and j["state"] == "running" for j in JOBS.values()):
            return 409, {"error": "já está rodando"}
        job = {"id": uuid.uuid4().hex[:10], "key": key, "action": name, "params": clean,
               "state": "running", "ok": None, "output": "", "started": time.time(), "ended": None}
        JOBS[job["id"]] = job
        _trim()
    t = threading.Thread(target=_worker, args=(job, act), daemon=True)
    t.start()
    if wait:
        t.join()
    return 202, {"job": job["id"], "action": name}


def job_view(job_id):
    j = JOBS.get(job_id)
    return None if j is None else {k: v for k, v in j.items() if k != "key"}


def jobs_view():
    return sorted((job_view(i) for i in list(JOBS)), key=lambda j: -j["started"])[:20]

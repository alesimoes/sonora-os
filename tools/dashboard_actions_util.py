"""Shared by the action modules: parameter checks and the orchestrator CLI."""
import json
import os
import sys

import dashboard_proc
from dashboard_actions import ActionError
from dashboard_util import ROOT, orch_root


def need_int(params, key):
    try:
        v = int(params.get(key))
    except (TypeError, ValueError):
        raise ActionError("%s must be a number" % key)
    if v <= 0:
        raise ActionError("%s must be positive" % key)
    return v


def need_str(params, key, maxlen=120):
    v = params.get(key)
    if not isinstance(v, str) or not v or len(v) > maxlen or any(c in v for c in "\r\n\0"):
        raise ActionError("%s must be a short text" % key)
    return v


def only(params, *keys):
    """Drops everything but the declared keys: a parameter nobody asked for never reaches a command."""
    return {k: params[k] for k in keys if k in params}


def orch_script():
    for base in (orch_root(), ROOT):
        p = os.path.join(base, "tools", "orchestrator.py")
        if os.path.exists(p):
            return p
    raise ActionError("tools/orchestrator.py not found")


def orch_cli(args, timeout):
    """python tools/orchestrator.py <args> in the daemon's checkout (rc, output)."""
    script = orch_script()
    return dashboard_proc.run_cmd([sys.executable, script, "--root", orch_root()] + list(args), timeout,
                                  cwd=os.path.dirname(os.path.dirname(script)))


def status_json():
    try:
        with open(os.path.join(orch_root(), "planning", "orchestrator", "status.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def result(rc, out):
    return rc == 0, out.strip() or ("ok" if rc == 0 else "exit %s" % rc)

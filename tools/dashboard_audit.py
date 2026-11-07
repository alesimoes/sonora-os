"""Audit log of the actions run from the dashboard: one JSON line per action,
.harness/dashboard-actions.log in the orchestrator's checkout."""
import json
import os
import threading
import time

from dashboard_util import orch_root

_lock = threading.Lock()
KEEP_OUTPUT = 600


def log_path():
    return os.environ.get("SONORA_DASH_AUDIT") or os.path.join(orch_root(), ".harness", "dashboard-actions.log")


def record(action, params, result, output, ok):
    row = {"ts": int(time.time()), "action": action, "params": params, "result": result,
           "ok": ok, "output": str(output or "")[-KEEP_OUTPUT:]}
    path = log_path()
    with _lock:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    return row


def recent(n=15):
    try:
        with open(log_path(), encoding="utf-8") as f:
            lines = f.readlines()[-n:]
    except OSError:
        return []
    rows = []
    for ln in lines:
        try:
            rows.append(json.loads(ln))
        except ValueError:
            continue
    return rows[::-1]

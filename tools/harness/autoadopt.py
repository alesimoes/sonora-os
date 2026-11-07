"""Adoption without stopping the daemon (owner, 2026-11-06).

Two doors, both run by the daemon's own tick so state.json has one writer:
- auto: every open, non-draft PR against main that no feature owns is adopted
  on the train's PR refresh cadence, event `adopted_pr` with detail `auto`;
- requests: `orchestrator.py adopt N` while the daemon runs drops a request
  file under .harness/adopt-requests/ and the next tick adopts it.
"""
import json
import os
import time

from .forge import ForgeError
from .status import write_atomic
from .train import LIST_TTL_S

REQUEST_DIR = "adopt-requests"
BASE = "main"
FRESH_S = 120.0     # status.json younger than this: a daemon is running


def request_dir(hdir):
    return os.path.join(hdir, REQUEST_DIR)


def daemon_alive(odir, now=None):
    try:
        age = (now or time.time()) - os.path.getmtime(
            os.path.join(odir, "status.json"))
    except OSError:
        return False
    return age < FRESH_S


def write_request(hdir, numbers):
    """Queue an adoption ([] means every open PR). Returns the file."""
    d = request_dir(hdir)
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, f"{time.time_ns()}-{os.getpid()}.json")
    write_atomic(path, {"prs": sorted(numbers)})
    return path


def take_requests(hdir):
    """[(numbers or None)] for each pending request; the files are removed."""
    d = request_dir(hdir)
    try:
        names = sorted(n for n in os.listdir(d) if n.endswith(".json"))
    except OSError:
        return []
    out = []
    for n in names:
        path = os.path.join(d, n)
        try:
            with open(path, encoding="utf-8") as f:
                prs = json.load(f).get("prs") or []
            os.remove(path)
        except (OSError, ValueError, AttributeError):
            continue    # half-written: the next tick sees it whole
        out.append({int(x) for x in prs} or None)
    return out


def tick(orch, now):
    """Requests always; the automatic sweep only for a live daemon."""
    for numbers in take_requests(orch.hdir):
        try:
            got = orch.train.adopt(numbers)
        except ForgeError as e:
            orch.store.event(None, "adopt_request_failed", str(e)[:200])
            continue
        orch.store.event(None, "adopt_request",
                         " ".join(f"#{n}" for n in got) or "none")
    if orch.dry_run or now - orch.auto_adopt_at < LIST_TTL_S:
        return
    orch.auto_adopt_at = now
    try:
        orch.train.adopt(auto=True)
    except ForgeError as e:   # incl. RateLimited: try again next cadence
        orch.store.event(None, "auto_adopt_failed", str(e)[:200])

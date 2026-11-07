"""Actions on local CI and the machine-wide heavy lock.

cancel a batch: only a batch state.json lists as running with a live pid;
its process tree is stopped and rc/summary say `cancelled`, so the daemon
reads a failure, never a pass (harness/localci_cancel.py).

remove a lock entry: only one tools/build-slot.sh --list shows, whose pid is
dead or whose command is not a build or test. The holders live in slot-N/owner
and the waiters in wait/<ticket>; the entry is the file naming that pid."""
import json
import os
import shutil

import dashboard_proc
from dashboard_actions import ActionError, register
from dashboard_actions_util import need_int, need_str, only
from dashboard_machine import GIT_BASH, LOCK_RE, heavy_lock_list
from dashboard_orch import orch_state
from harness import launcher, localci_cancel

REAL_JOB_MARKERS = ("build-", "local-ci", "docker", "ctest", "cmake", "test-", "pre-pr", "sonora")


def running_batches():
    out = {}
    for k, v in (orch_state().get("meta") or {}).items():
        if not k.startswith("localci:"):
            continue
        try:
            r = json.loads(v)
        except ValueError:
            continue
        if r.get("state") == "running" and r.get("pid") and r.get("dir") and not r.get("batch"):
            out[k[len("localci:"):]] = r
    return out


def _v_cancel(p):
    p = only(p, "key")
    need_str(p, "key")
    r = running_batches().get(p["key"])
    if r is None:
        raise ActionError("não há lote de CI local rodando com essa chave")
    if not launcher.pid_alive(r["pid"]):
        raise ActionError("o processo do lote já terminou")
    return p


def _cancel(p, timeout):
    r = running_batches().get(p["key"])
    if r is None:
        return False, "o lote já terminou"
    localci_cancel.kill_tree(r["pid"])
    localci_cancel.mark_cancelled_files(r["dir"])
    return True, "lote cancelado (pid %s); o resultado fica como falha, nunca como verde" % r["pid"]


# -- the lock ---------------------------------------------------------------
def lock_dir():
    if os.environ.get("SONORA_HEAVY_DIR"):
        return os.environ["SONORA_HEAVY_DIR"]
    if os.name == "nt":
        return os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/AppData/Local"), "sonora", "build-lock")
    return "/var/tmp/sonora-build-lock"


def lock_entries():
    rows = []
    for ln in heavy_lock_list():
        m = LOCK_RE.match(ln.strip())
        if m:
            rows.append({"role": m.group(1), "pid": int(m.group(2)), "kind": m.group(3), "cmd": m.group(8).strip()})
    return rows


def pid_dead(entry):
    """True only when sure: a pid of the other kind of shell is probed with that shell's own kill -0."""
    rc, _ = dashboard_proc.run_cmd([GIT_BASH or "bash", "-c", "kill -0 %d" % entry["pid"]], 10)
    return rc == 1


def removable(entry):
    if pid_dead(entry):
        return "o pid %d morreu" % entry["pid"]
    cmd = entry["cmd"]
    if not any(m in cmd for m in REAL_JOB_MARKERS):
        return "o comando não é um build nem um teste: %s" % cmd[:60]
    return None


def _v_lock(p):
    p = only(p, "pid")
    pid = need_int(p, "pid")
    entry = next((e for e in lock_entries() if e["pid"] == pid), None)
    if entry is None:
        raise ActionError("o pid %d não está na fila de trava" % pid)
    why = removable(entry)
    if why is None:
        raise ActionError("o pid %d é um trabalho de verdade e está vivo; a trava não o remove" % pid)
    return {"pid": pid}


def _entry_paths(pid):
    base = lock_dir()
    cands = []
    for d in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        path = os.path.join(base, d)
        if d.startswith("slot-"):
            cands.append((path, os.path.join(path, "owner")))
        elif d == "wait" and os.path.isdir(path):
            cands += [(os.path.join(path, t),) * 2 for t in sorted(os.listdir(path))]
    for target, owner in cands:
        try:
            with open(owner, encoding="utf-8", errors="replace") as f:
                mine = ("pid=%d" % pid) in f.read().split()
        except OSError:
            continue
        if mine:    # yielded after the file is closed: Windows cannot delete an open file
            yield target


def _remove_lock(p, timeout):
    gone = []
    for t in _entry_paths(p["pid"]):
        shutil.rmtree(t) if os.path.isdir(t) else os.remove(t)
        gone.append(os.path.basename(t))
    return bool(gone), ("removido: " + ", ".join(gone)) if gone else "nenhuma entrada com esse pid"


register("ci.cancel", "ci", "cancelar lote", _v_cancel, _cancel, destructive=True, timeout=45)
register("lock.remove", "ci", "remover da fila de trava", _v_lock, _remove_lock, destructive=True, timeout=20)

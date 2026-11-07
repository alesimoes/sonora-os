"""Actions on the pools, the daemon, the supervisor and the disk.

pools: the orchestrator's own `pause --pool X [--resume]`; X must be a pool
that status.json lists.
daemon: status; restart (stop the pid, start `orchestrator.py run --live`
detached in its checkout); update (fast-forward the checkout to origin/main,
refused when it has uncommitted changes, then restart).
supervisor: start or stop tools/supervisor.py when the checkout has it.
disk: the janitor is tools/clean.sh, which removes only what is ours (label
org.sonora=1, sonora-* names, merged worktrees); preview is its --dry-run."""
import os
import sys
import time

import dashboard_proc
from dashboard_actions import ActionError, register
from dashboard_actions_util import need_str, only, orch_cli, orch_script, result, status_json
from dashboard_machine import GIT_BASH
from dashboard_util import main_root, orch_root
from harness import launcher, proctree


# -- pools -------------------------------------------------------------------
def _v_pool(p):
    p = only(p, "pool")
    need_str(p, "pool", 40)
    if p["pool"] not in (status_json().get("pools") or {}):
        raise ActionError("pool desconhecido: %s" % p["pool"])
    return p


register("pool.pause", "pools", "pausar pool", _v_pool,
         lambda p, t: result(*orch_cli(["pause", "--pool", p["pool"]], t)), timeout=30)
register("pool.resume", "pools", "retomar pool", _v_pool,
         lambda p, t: result(*orch_cli(["pause", "--pool", p["pool"], "--resume"], t)), timeout=30)


# -- processes ---------------------------------------------------------------
def daemon_pids():
    return [pid for pid, args in dashboard_proc.find_processes(["orchestrator.py", " run", "--live"])]


def stop_pids(pids):
    for pid in pids:
        proctree.kill_tree(pid)
    end = time.time() + 20
    while time.time() < end and any(launcher.pid_alive(p) for p in pids):
        time.sleep(0.5)
    return not any(launcher.pid_alive(p) for p in pids)


def start_daemon():
    root = orch_root()
    log = os.path.join(root, ".harness", "daemon.log")
    return dashboard_proc.spawn_detached([sys.executable, orch_script(), "run", "--live"], root, log)


def _restart(p, timeout):
    pids = daemon_pids()
    if len(pids) > 1:
        return False, "mais de um daemon encontrado (%s); nada foi feito" % pids
    if pids and not stop_pids(pids):
        return False, "o daemon %s não encerrou em 20 s" % pids
    return True, "daemon parado (%s); novo pid %d" % (pids or "não rodava", start_daemon())


def _update(p, timeout):
    root = orch_root()
    rc, out = dashboard_proc.run_cmd(["git", "-C", root, "status", "--porcelain", "--untracked-files=no"], 30)
    if rc != 0 or out.strip():
        return False, "o checkout do daemon tem alterações locais ou não pôde ser lido; nada foi feito\n" + out
    for cmd in (["git", "-C", root, "fetch", "origin", "main"], ["git", "-C", root, "merge", "--ff-only", "origin/main"]):
        rc, out = dashboard_proc.run_cmd(cmd, timeout)
        if rc != 0:
            return False, "falhou: %s\n%s" % (" ".join(cmd[3:]), out)
    return _restart(p, timeout)


def _daemon_status(p, timeout):
    rc, out = orch_cli(["status"], timeout)
    pids = daemon_pids()
    return rc == 0, "pids do daemon: %s\n%s" % (pids or "nenhum", out[:1500])


def _no_params(p):
    return {}


register("daemon.status", "daemon", "status do daemon", _no_params, _daemon_status, timeout=30)
register("daemon.restart", "daemon", "reiniciar daemon", _no_params, _restart, destructive=True, timeout=60)
register("daemon.update", "daemon", "atualizar para a main e reiniciar", _no_params, _update, destructive=True, timeout=180)


# -- supervisor --------------------------------------------------------------
def _supervisor_script():
    p = os.path.join(orch_root(), "tools", "supervisor.py")
    if not os.path.exists(p):
        raise ActionError("tools/supervisor.py não existe neste checkout")
    return p


def supervisor_pids():
    return [pid for pid, _ in dashboard_proc.find_processes(["supervisor.py"])]


def _v_sup(p):
    _supervisor_script()
    return {}


def _sup_start(p, timeout):
    if supervisor_pids():
        return False, "o supervisor já está rodando"
    log = os.path.join(orch_root(), ".harness", "supervisor.out.log")
    return True, "supervisor iniciado, pid %d" % dashboard_proc.spawn_detached(
        [sys.executable, _supervisor_script(), "--root", orch_root()], orch_root(), log)


def _sup_stop(p, timeout):
    pids = supervisor_pids()
    if not pids:
        return False, "o supervisor não está rodando"
    return stop_pids(pids), "supervisor parado: %s" % pids


register("supervisor.start", "supervisor", "iniciar supervisor", _v_sup, _sup_start, timeout=30)
register("supervisor.stop", "supervisor", "parar supervisor", _v_sup, _sup_stop, destructive=True, timeout=45)


# -- disk --------------------------------------------------------------------
MODES = {"": [], "worktrees": ["--worktrees-only"]}


def _v_clean(p):
    p = only(p, "mode")
    p.setdefault("mode", "")
    if p["mode"] not in MODES:
        raise ActionError("mode must be one of %s" % sorted(MODES))
    return p


def clean_cmd(p, dry):
    script = os.path.join(main_root(), "tools", "clean.sh")
    if not os.path.exists(script):
        raise ActionError("tools/clean.sh não existe")
    return [GIT_BASH or "bash", script] + (["--dry-run"] if dry else []) + MODES[p["mode"]]


def _clean(dry):
    def go(p, timeout):
        return result(*dashboard_proc.run_cmd(clean_cmd(p, dry), timeout, cwd=main_root()))
    return go


register("disk.preview", "disk", "ver o que a limpeza liberaria", _v_clean, _clean(True), timeout=300)
register("disk.clean", "disk", "limpar o disco", _v_clean, _clean(False), destructive=True, timeout=1800)

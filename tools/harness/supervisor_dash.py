"""Supervisor check: the dashboard must answer on :8765. When it does not
for two sweeps in a row it is started again from its own checkout (default
<repo>/.claude/worktrees/dash-live), keeping SONORA_ORCH_STATUS and the
rest of the environment. A start is retried at most once per RESTART_MIN_S.
The sys surface is dash_up(port) / dash_start(port); a sys without it
(old fakes) skips the check."""
import os
import subprocess
import sys
import urllib.request

PORT = 8765
RESTART_MIN_S = 300.0
FAILS_NEEDED = 2


def dash_root(repo_root, env=None):
    env = env if env is not None else os.environ
    if env.get("SONORA_DASH_ROOT"):
        return env["SONORA_DASH_ROOT"]
    parent = os.path.dirname(os.path.abspath(repo_root))
    if os.path.basename(parent) == "worktrees":   # the daemon's own worktree
        return os.path.join(parent, "dash-live")
    return os.path.join(repo_root, ".claude", "worktrees", "dash-live")


def probe(port=PORT, timeout=3.0):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/",
                                    timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


def start(repo_root, port=PORT, popen=subprocess.Popen, env=None):
    """Returns the command line started, or None when there is no checkout."""
    script = os.path.join(dash_root(repo_root, env), "tools", "dashboard.py")
    if not os.path.exists(script):
        return None
    cmd = [sys.executable, script, "--port", str(port)]
    # CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP: no visible console for it
    # or its children (DETACHED_PROCESS made each child open a window).
    flags = 0x08000000 | 0x00000200 if os.name == "nt" else 0
    popen(cmd, cwd=os.path.dirname(os.path.dirname(script)),
          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
          creationflags=flags, start_new_session=os.name != "nt")
    return cmd


def check_dashboard(sv, now):
    up = getattr(sv.sys, "dash_up", None)
    if up is None:
        return
    if up(PORT):
        sv.mem.pop("dash", None)
        return
    fails = sv.remember("dash", "fails", 0) + 1
    sv.mem["dash"]["fails"] = fails
    if fails < FAILS_NEEDED:
        return
    last = sv.remember("dash", "started", -RESTART_MIN_S)
    if now - last < RESTART_MIN_S:
        return
    sv.mem["dash"]["started"] = now
    sv.act("dashboard", "restart dashboard",
           f":{PORT} did not answer {fails} sweeps",
           lambda: sv.sys.dash_start(PORT, sv.o.root) is not None)

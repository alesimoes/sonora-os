"""The dashboard follows main like the daemon does (harness/selfupdate.py).

It ran #482's code for hours after later PRs had merged. Every CHECK_S a
background thread fetches origin/main in the dashboard's own checkout; when
tools/dashboard* differ from HEAD it fast-forwards that checkout (detached,
refused when tracked files are modified) and re-execs the process with the
same argv and environment, so port, SONORA_ORCH_STATUS and the rest stay.
Bounded: one fetch per CHECK_S, git calls time out, every step is logged.
"""
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness.selfupdate import _git, reexec  # noqa: E402

WATCH = ("tools/dashboard*", "tools/harness/console*")
CHECK_S = 300
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _log(msg):
    print("[dashboard-selfupdate] " + msg, flush=True)


def changed(root=ROOT, git=_git):
    if git(["fetch", "-q", "origin", "main"], root, 60).returncode:
        return []
    r = git(["diff", "--name-only", "HEAD", "origin/main", "--"]
            + list(WATCH), root, 60)
    return [f for f in r.stdout.splitlines() if f] if not r.returncode else []


def update_once(root=ROOT, git=_git, restart=reexec, argv=None, log=_log):
    """One check. Returns 'current', 'blocked' or 'restarted'."""
    files = changed(root, git)
    if not files:
        return "current"
    dirty = git(["status", "--porcelain", "--untracked-files=no"],
                root, 60).stdout.strip()
    if dirty:
        log("update refused, tracked files modified: " + dirty[:200])
        return "blocked"
    r = git(["checkout", "-q", "--detach", "origin/main"], root, 60)
    if r.returncode:
        log("checkout failed: " + r.stderr.strip()[:200])
        return "blocked"
    log("updated to origin/main (" + " ".join(files)[:200] + "); restarting")
    argv = argv or [os.path.abspath(sys.argv[0])] + sys.argv[1:]
    restart(argv)
    return "restarted"


def loop(root=ROOT, interval=CHECK_S, sleep=time.sleep, **kw):
    while True:
        sleep(interval)
        try:
            update_once(root, **kw)
        except Exception as e:   # an update check never stops the dashboard
            _log("check failed: " + repr(e)[:200])


def start(root=ROOT):
    threading.Thread(target=loop, args=(root,), daemon=True).start()

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

# Static floor; watched() adds every tools/ module this process imported, so
# a new import is covered without anyone editing this list.
WATCH = ("tools/dashboard*", "tools/harness/console*", "tools/progress.py",
         "tools/harness/launcher.py")
CHECK_S = 300
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _log(msg):
    print("[dashboard-selfupdate] " + msg, flush=True)


def watched(root=ROOT, modules=None):
    """WATCH plus the repo files of every module this process has loaded."""
    mods = sys.modules.values() if modules is None else modules
    tools = os.path.join(root, "tools") + os.sep
    found = set()
    for m in list(mods):
        f = os.path.abspath(getattr(m, "__file__", None) or "")
        if f.startswith(tools) and f.endswith(".py"):
            found.add(os.path.relpath(f, root).replace(os.sep, "/"))
    return list(WATCH) + sorted(found)


def loaded_sha(root=ROOT, git=_git):
    """The commit this process's code came from. The daemon's own self-update
    moves the shared checkout first, so HEAD says nothing about what runs
    here (2026-11-13: the dashboard never restarted after a merge)."""
    r = git(["rev-parse", "HEAD"], root, 60)
    return r.stdout.strip() if not r.returncode else "HEAD"


def changed(root=ROOT, git=_git, loaded="HEAD"):
    if git(["fetch", "-q", "origin", "main"], root, 60).returncode:
        return []
    r = git(["diff", "--name-only", loaded, "origin/main", "--"]
            + watched(root), root, 60)
    return [f for f in r.stdout.splitlines() if f] if not r.returncode else []


def update_once(root=ROOT, git=_git, restart=reexec, argv=None, log=_log,
                loaded="HEAD"):
    """One check. Returns 'current', 'blocked' or 'restarted'."""
    files = changed(root, git, loaded)
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
    threading.Thread(target=loop, args=(root,), daemon=True,
                     kwargs={"loaded": loaded_sha(root)}).start()

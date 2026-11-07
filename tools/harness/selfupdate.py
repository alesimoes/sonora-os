"""The daemon follows main's harness (2026-11-07: it ran day-old code, so a
fix already merged, auto-adopt, was not running, and the coordinator had
to notice and restart it by hand).

Every CHECK_S the live daemon fetches origin/main and diffs it with its own
checkout's HEAD on WATCH: the daemon's code and the lock script. When they
differ:
  1. the checkout moves to origin/main (detached; refused, with an event,
     when tracked files are modified);
  2. the store is flushed, the broker stopped, and the process re-execs
     itself with the same arguments. State is on disk; agents still running
     are adopted by the new daemon (Orchestrator.adopt_runs).
"""
import os
import subprocess
import sys

WATCH = ("tools/harness", "tools/orchestrator.py", "tools/build-slot.sh")
CHECK_S = 300
CODE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


def _git(args, cwd, timeout=120):
    return subprocess.run(["git"] + args, cwd=cwd, capture_output=True,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


def reexec(argv, exe=None):
    """Replaces this process with a fresh one running argv. On Windows
    os.execv mangles arguments with spaces, so a child with the same
    console and handles starts and this process exits."""
    exe = exe or sys.executable
    if os.name == "nt":
        subprocess.Popen([exe] + list(argv))
        os._exit(0)
    os.execv(exe, [exe] + list(argv))


class SelfUpdate:
    def __init__(self, orch, argv, root=None, git=_git, restart=reexec,
                 release=None):
        self.o = orch
        self.argv = [os.path.abspath(argv[0])] + list(argv[1:])
        self.root = root or CODE_ROOT
        self.git = git
        self._restart = restart
        self._release = release or (lambda: None)
        self.checked = 0.0
        self.pending = []
        self.blocked = None

    def changed(self):
        """WATCH files that differ between HEAD and origin/main."""
        if self.git(["fetch", "-q", "origin", "main"], self.root).returncode:
            return []
        r = self.git(["diff", "--name-only", "HEAD", "origin/main", "--"]
                     + list(WATCH), self.root)
        return [f for f in r.stdout.splitlines() if f] if not r.returncode \
            else []

    def update_checkout(self):
        """Moves the checkout to origin/main; False (and one event per
        target) when tracked files are modified or the checkout fails."""
        target = self.git(["rev-parse", "origin/main"], self.root).stdout.strip()
        dirty = self.git(["status", "--porcelain", "--untracked-files=no"],
                         self.root).stdout.strip()
        why = f"tracked files modified: {dirty[:200]}" if dirty else None
        if not why:
            r = self.git(["checkout", "-q", "--detach", "origin/main"],
                         self.root)
            why = r.stderr.strip()[:200] if r.returncode else None
        if why:
            if self.blocked != target:
                self.o.store.event("daemon", "self_update_blocked", why)
            self.blocked = target
            return False
        self.o.store.event("daemon", "self_update",
                           f"{target[:12]}: " + " ".join(self.pending)[:200])
        return True

    def ready(self, now):
        """True when the checkout is on the new main and the process may
        re-exec now."""
        if not self.pending:
            if now - self.checked < CHECK_S:
                return False
            self.checked = now
            self.pending = self.changed()
            if not self.pending:
                return False
            self.o.store.event("daemon", "self_update_pending",
                               " ".join(self.pending)[:200])
        if not self.update_checkout():
            self.pending = []
            return False
        return True

    def restart(self):
        """Flushes, stops the broker, hands the lock over, re-execs."""
        self.o.store.flush()
        try:
            self.o.broker.stop()
        except Exception:
            pass
        if os.name == "nt":
            self._release()   # the child is a new pid; on POSIX it is ours
        self._restart(self.argv)

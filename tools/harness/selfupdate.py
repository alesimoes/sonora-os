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

Bounded wait (2026-11-07: a daemon stayed on old code for over an hour and
kept launching deleted scripts). Nothing may defer the re-exec for longer
than max_defer_s (Config.self_update_max_defer_s, default 600) from the
moment the change is first seen: an optional `defer_reason()` can hold the
re-exec back, and a dirty checkout blocks the move; once the bound passes
the checkout is forced to origin/main (the event names what was discarded)
and the process re-execs. Agent runs are separate processes recorded in
the store; the new daemon re-attaches to them with adopt_runs.
"""
import os
import subprocess
import sys

# The dashboard is served from this checkout too: a dashboard-only merge must
# move it, or the owner keeps the old page (2026-11-08, #571 never showed).
WATCH = ("tools/harness", "tools/orchestrator.py", "tools/build-slot.sh",
         "tools/dashboard*",
         # the daemon launches agents from this checkout's definitions
         ".claude/agents")
CHECK_S = 300
# Local commits the planners may leave in the daemon checkout; anything else
# there is a mistake that must be seen, not rebased under the daemon's feet.
PLAN_ONLY = ("planning/plans/", "brain/", ".harness/")
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
                 release=None, max_defer_s=600.0, defer_reason=None):
        self.o = orch
        self.argv = [os.path.abspath(argv[0])] + list(argv[1:])
        self.root = root or CODE_ROOT
        self.git = git
        self._restart = restart
        self._release = release or (lambda: None)
        self.checked = 0.0
        self.pending = []
        self.blocked = None
        self.max_defer_s = max_defer_s
        self.defer_reason = defer_reason or (lambda: None)
        self.first_seen = None
        self.now = 0.0

    def changed(self):
        """WATCH files that differ between HEAD and origin/main."""
        if self.git(["fetch", "-q", "origin", "main"], self.root).returncode:
            return []
        r = self.git(["diff", "--name-only", "HEAD", "origin/main", "--"]
                     + list(WATCH), self.root)
        return [f for f in r.stdout.splitlines() if f] if not r.returncode \
            else []

    def overdue(self):
        return self.first_seen is not None and             self.now - self.first_seen >= self.max_defer_s

    def update_checkout(self):
        """Moves the checkout to origin/main; False (and one event per
        target) when tracked files are modified or the checkout fails."""
        target = self.git(["rev-parse", "origin/main"], self.root).stdout.strip()
        old = self.git(["rev-parse", "HEAD"], self.root).stdout.strip()
        dirty = self.git(["status", "--porcelain", "--untracked-files=no"],
                         self.root).stdout.strip()
        force = self.overdue()
        why = f"tracked files modified: {dirty[:200]}"             if dirty and not force else None
        if dirty and force:
            self.o.store.event("daemon", "self_update_forced",
                               f"discarding: {dirty[:200]}")
        if not why:
            why = self._move_to(target, force)
        if why:
            if self.blocked != target:
                self.o.store.event("daemon", "self_update_blocked", why)
            self.blocked = target
            return False
        self.o.store.event("daemon", "self_update",
                           f"{old[:12]} -> {target[:12]}: " + " ".join(self.pending)[:200])
        return True

    def _move_to(self, target, force):
        """Checkout origin/main, keeping local commits. Planners commit plans
        in this checkout (2026-11-08: five plan commits); a bare checkout
        would drop them, so they are rebased onto origin/main instead. A
        rebase conflict is aborted and reported, never resolved here."""
        local = self.git(["rev-list", "origin/main..HEAD"],
                         self.root).stdout.split()
        if local:
            foreign = self._non_plan_files()
            if foreign:
                return (f"ALERT {len(local)} local commit(s) in the daemon "
                        f"checkout touch more than plans: "
                        f"{' '.join(foreign)[:150]}; move them to a branch")
            r = self.git(["-c", "user.name=sonora-harness",
                          "-c", "user.email=harness@sonora.invalid",
                          "rebase", "-q", "origin/main"], self.root)
            if r.returncode:
                self.git(["rebase", "--abort"], self.root)
                return (f"rebase of {len(local)} local commit(s) onto "
                        f"origin/main failed: {r.stderr.strip()[:150]}")
            self.o.store.event("daemon", "self_update_rebased",
                               f"{len(local)} local commit(s) kept")
            return None
        r = self.git(["checkout", "-q", "--detach"]
                     + (["-f"] if force else []) + [target], self.root)
        return r.stderr.strip()[:200] if r.returncode else None

    def _non_plan_files(self):
        r = self.git(["diff", "--name-only", "origin/main...HEAD"], self.root)
        return [f for f in r.stdout.splitlines()
                if f and not f.startswith(PLAN_ONLY)]

    def ready_or_report(self, now):
        """ready(), but an exception becomes one event per distinct error
        instead of a traceback nobody reads (the daemon's stderr is
        discarded; 2026-11-08 it stayed on old code for hours)."""
        try:
            return self.ready(now)
        except Exception as e:
            msg = repr(e)[:300]
            if msg != getattr(self, "_last_error", None):
                self._last_error = msg
                self.o.store.event("daemon", "self_update_error", msg)
            return False

    def ready(self, now):
        """True when the checkout is on the new main and the process may
        re-exec now."""
        self.now = now
        if not self.pending:
            if now - self.checked < CHECK_S:
                return False
            self.checked = now
            self.pending = self.changed()
            if not self.pending:
                return False
            self.first_seen = now
            self.o.store.event("daemon", "self_update_pending",
                               " ".join(self.pending)[:200])
        reason = self.defer_reason()
        if reason and not self.overdue():
            return False
        if not self.update_checkout():
            if not self.overdue():
                return False   # retried each tick until the bound forces it
            self.pending, self.first_seen = [], None
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

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

from . import runtime_files
from . import selfupdate_local as L
from . import selfupdate_runtime

# The dashboard is served from this checkout too: a dashboard-only merge must
# move it, or the owner keeps the old page (2026-11-08, #571 never showed).
WATCH = ("tools/harness", "tools/orchestrator.py", "tools/build-slot.sh",
         "tools/dashboard*",
         # the daemon launches agents from this checkout's definitions
         ".claude/agents")
CHECK_S = 300
# Local commits the planners may leave in the daemon checkout; anything else
# there is a mistake that must be seen, not rebased under the daemon's feet.
PLAN_ONLY = L.PLAN_ONLY
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
        dirty = self._dirty()
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

    def _dirty(self):
        """Modified tracked files, runtime files left out: those are kept
        across the move by selfupdate_runtime, so they never block it."""
        out = self.git(["status", "--porcelain", "--untracked-files=no"],
                       self.root).stdout
        keep = [ln for ln in out.splitlines()
                if ln.strip() and not runtime_files.is_runtime(
                    ln[3:].split(" -> ")[-1].strip('"'))]
        return "\n".join(keep).strip()

    def _move_to(self, target, force):
        """Checkout origin/main, keeping local commits. Planners commit plans
        in this checkout (2026-11-08: five plan commits); a bare checkout
        would drop them, so they are rebased onto origin/main instead.
        Runtime files survive the move; untracked files main now carries
        are moved aside first (2026-11-13: an uncommitted ADR blocked it)."""
        runtime = selfupdate_runtime.Runtime(self.root, self.git, self.now)
        try:
            self._aside_untracked(target)
            return self._move_commits(target, force)
        finally:
            self._restored(runtime.restore(), runtime.aside)

    def _restored(self, rels, aside):
        """2026-11-14: git deleted state.json and events.jsonl in the move.
        What it took is back; the store rewrites state.json from memory."""
        if not rels:
            return
        self.o.store.event("daemon", "self_update_runtime_restored",
                           f"ALERT git removed or changed "
                           f"{' '.join(rels)[:150]}; restored (copy in "
                           f"{aside})")
        mark = getattr(self.o.store, "mark_dirty", None)
        if mark:
            mark()

    def _move_commits(self, target, force):
        local = self.git(["rev-list", "origin/main..HEAD"],
                         self.root).stdout.split()
        if local:
            foreign = self._non_plan_files()
            carried = self._runtime_files()
            if foreign and not force:
                return (f"ALERT {len(local)} local commit(s) in the daemon "
                        f"checkout touch more than plans: "
                        f"{' '.join(foreign)[:150]}; saved on a keep/ "
                        f"branch and dropped at the bound")
            if foreign or carried:
                # a rebase would keep runtime files tracked: strip them now
                return self._drop_foreign(len(local), target,
                                          foreign + carried)
            return self._rebase(len(local), target, force)
        return self._checkout(target, force)

    def _aside_untracked(self, target):
        moved, dest = L.move_aside_untracked(self.git, self.root, target,
                                             self.now)
        if moved:
            self.o.store.event("daemon", "self_update_aside",
                               f"{len(moved)} untracked file(s) main carries "
                               f"moved to {dest}: {' '.join(moved)[:150]}")

    def _drop_foreign(self, n, target, foreign):
        """2026-11-12/13: a product commit and a tracked paused.json in this
        checkout blocked the update ~8 h, the event asking for a branch
        nobody made. Past the bound the harness makes it: HEAD on a keep/
        branch, the checkout on main, the plan-only commits replayed."""
        commits = L.local_commits(self.git, self.root)
        saved, err = L.save_head(self.git, self.root, self.now)
        if err:
            return err
        why = self._checkout(target, True)
        if why:
            return why
        plans = [c for c in commits
                 if L.plan_only(L.commit_files(self.git, self.root, c))]
        applied, skipped = L.replay(self.git, self.root, plans)
        self.o.store.event(
            "daemon", "self_update_forced",
            f"ALERT {n} local commit(s) touched more than plans "
            f"({' '.join(foreign)[:120]}); dropped from the daemon checkout, "
            f"{applied} plan commit(s) replayed, {skipped} skipped; "
            f"saved on {saved}")
        return None

    def _checkout(self, target, force):
        r = self.git(["checkout", "-q", "--detach"]
                     + (["-f"] if force else []) + [target], self.root)
        return r.stderr.strip()[:200] if r.returncode else None

    def _rebase(self, n, target, force):
        """A plan or ADR a feature PR carried to main, edited in review,
        conflicts with the planner's local commit of the same file
        (2026-11-09: aborted every 5 min for hours, so #607 never ran).
        Main holds the reviewed text, so its side wins conflicting hunks
        (`-X ours`: in a rebase "ours" is the upstream). What even that
        cannot resolve is saved on a branch once the bound passes and the
        checkout goes to main: the harness must not stay old for a plan."""
        err = ""
        for extra, how in (([], "kept"),
                           (["-X", "ours"], "kept, main's version won "
                                            "the conflicting hunks")):
            r = self.git(["-c", "user.name=sonora-harness",
                          "-c", "user.email=harness@sonora.invalid",
                          "rebase", "-q"] + extra + ["origin/main"], self.root)
            if not r.returncode:
                self.o.store.event("daemon", "self_update_rebased",
                                   f"{n} local commit(s) {how}")
                return None
            self.git(["rebase", "--abort"], self.root)
            err = err or r.stderr.strip()[:150]
        if not force:
            return (f"rebase of {n} local commit(s) onto origin/main "
                    f"failed: {err}")
        return self._save_and_checkout(n, target)

    def _save_and_checkout(self, n, target):
        saved, err = L.save_head(self.git, self.root, self.now)
        if err:
            return err
        self.o.store.event("daemon", "self_update_forced",
                           f"ALERT {n} local commit(s) did not rebase; "
                           f"saved on {saved}")
        return self._checkout(target, True)

    def _local_files(self):
        r = self.git(["diff", "--name-only", "origin/main...HEAD"], self.root)
        return [f for f in r.stdout.splitlines() if f]

    def _non_plan_files(self):
        return L.foreign(self._local_files())

    def _runtime_files(self):
        """Runtime files the local commits track (they must never be)."""
        return runtime_files.runtime_only(self._local_files())

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

"""The processes and git calls behind the screenshot job (harness/shots).

Kept apart so the job's logic is tested against a fake and this file against
a scratch git repository."""
import os
import subprocess

from . import launcher, shell

SHOT_FILE = "docs/images/sonora-ui-edit.png"
IMAGES = "docs/images"


def _git(worktree, *args, timeout=120):
    return subprocess.run(["git", "-C", worktree, *args], capture_output=True,
                          text=True, timeout=timeout)


class RealIO:
    def __init__(self, base_ref="origin/main"):
        self.base_ref = base_ref

    def head(self, worktree):
        return launcher.git_head(worktree)

    def changed(self, worktree):
        r = _git(worktree, "diff", "--name-only", f"{self.base_ref}...HEAD")
        return r.stdout.split() if r.returncode == 0 else []

    def start(self, worktree, log, rc):
        """Run the regeneration detached, through the machine-wide build
        slot; the exit code lands in `rc`. Returns the pid. A daemon
        restart does not stop it, and the daemon never waits on it."""
        script = ("tools/build-slot.sh tools/build-ui.sh --screenshot "
                  f'{SHOT_FILE} > "{log}" 2>&1; echo $? > "{rc}"')
        kw = {}
        if os.name == "nt":
            kw["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                                   | getattr(subprocess, "DETACHED_PROCESS", 0))
        else:
            kw["start_new_session"] = True
        p = subprocess.Popen([shell.bash(), "-c", script], cwd=worktree,
                             stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, **kw)
        return p.pid

    def alive(self, pid):
        return launcher.pid_alive(pid)

    def commit(self, worktree):
        """Commit what the run wrote under docs/images; True when there was
        something to commit."""
        _git(worktree, "add", "-A", "--", IMAGES)
        if _git(worktree, "diff", "--cached", "--quiet").returncode == 0:
            return False
        r = _git(worktree, "-c", "user.name=sonora-harness",
                 "-c", "user.email=harness@sonora.invalid", "commit", "-m",
                 "ui: regenerate screenshots (daemon screenshot job)\n\n"
                 "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>")
        if r.returncode != 0:
            raise RuntimeError("git commit failed: " + (r.stderr or "")[-300:])
        return True

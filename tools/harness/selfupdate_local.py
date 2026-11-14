"""What the daemon checkout holds that origin/main does not, and how the
self-update gets past it (2026-11-12/13: blocked ~8 h, so merged harness
fixes such as #673 never ran).

Three things stopped it, each handled here so selfupdate.py stays a loop:
  - a local commit outside plans (a planner committed runtime state, an
    agent committed product code in the daemon checkout). Once the bound
    passes, HEAD is saved on `keep/daemon-<date>-<sha>`, the checkout goes
    to origin/main and the plan-only commits are replayed on top
    (keep_branch, commit_files, replay);
  - an untracked file origin/main now carries (a planner's ADR written but
    not committed): git refuses the checkout, so it is moved under
    .harness/selfupdate-aside/ first (move_aside_untracked);
  - runtime files a local commit tracked: dropping that commit would
    delete them (the owner's paused.json), so they are kept across the
    move (Runtime).
Every git call goes through the injected `git(args, cwd)`.
"""
import os
import shutil
import time

# Local commits the planners may leave; anything else is foreign.
PLAN_ONLY = ("planning/plans/", "brain/", ".harness/")
# Runtime state written into the checkout, never code (.gitignore'd).
RUNTIME = ("planning/orchestrator/paused.json",)
ASIDE = os.path.join(".harness", "selfupdate-aside")
IDENTITY = ["-c", "user.name=sonora-harness",
            "-c", "user.email=harness@sonora.invalid"]


def _stamp(now, fmt):
    return time.strftime(fmt, time.localtime(now))


def keep_branch(head, now):
    return f"keep/daemon-{_stamp(now, '%Y%m%d')}-{head[:12]}"


def save_head(git, root, now):
    """(branch, None) or (None, error): HEAD kept on a keep/ branch."""
    head = git(["rev-parse", "HEAD"], root).stdout.strip()
    name = keep_branch(head, now)
    r = git(["branch", "-f", name, head], root)
    if r.returncode:
        return None, f"could not save local commits: {r.stderr.strip()[:150]}"
    return name, None


def plan_only(files):
    return all(f.startswith(PLAN_ONLY) for f in files)


def commit_files(git, root, sha):
    r = git(["show", "--name-only", "--format=", sha], root)
    return [f for f in r.stdout.splitlines() if f]


def local_commits(git, root):
    """Oldest first: the commits on HEAD that origin/main lacks."""
    return git(["rev-list", "--reverse", "origin/main..HEAD"],
               root).stdout.split()


def replay(git, root, commits):
    """Cherry-picks each commit, main's side winning conflicting hunks as
    in the rebase; one that still does not apply (or is already on main)
    is skipped: it is on the keep branch. Returns (applied, skipped)."""
    applied = skipped = 0
    for sha in commits:
        r = git(IDENTITY + ["cherry-pick", "-X", "ours", sha], root)
        if r.returncode:
            git(["cherry-pick", "--abort"], root)
            skipped += 1
        else:
            applied += 1
    return applied, skipped


def _z(out):
    return [f for f in out.split("\0") if f]


def untracked_in(git, root, target):
    """Untracked, not ignored files of the checkout that target tracks."""
    mine = _z(git(["ls-files", "-z", "--others", "--exclude-standard"],
                  root).stdout)
    if not mine:
        return []
    theirs = set(_z(git(["ls-tree", "-r", "-z", "--name-only", target],
                        root).stdout))
    return [f for f in mine if f in theirs]


def move_aside_untracked(git, root, target, now):
    """(moved files, destination) — empty when nothing is in the way."""
    files = untracked_in(git, root, target)
    if not files:
        return [], None
    dest = os.path.join(root, ASIDE, _stamp(now, "%Y%m%d-%H%M%S"))
    for rel in files:
        to = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.move(os.path.join(root, rel), to)
    return files, dest


class Runtime:
    """Snapshot of RUNTIME files; restore() writes back the ones a move
    deleted, never overwriting one that is there."""

    def __init__(self, root):
        self.root, self.saved = root, {}
        for rel in RUNTIME:
            try:
                with open(os.path.join(root, rel), "rb") as f:
                    self.saved[rel] = f.read()
            except OSError:
                pass

    def restore(self):
        for rel, data in self.saved.items():
            p = os.path.join(self.root, rel)
            if not os.path.exists(p):
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(data)

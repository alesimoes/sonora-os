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
    delete them (the owner's paused.json, 2026-11-14 state.json and
    events.jsonl), so they are kept across the move
    (selfupdate_runtime.Runtime) and stripped from every replayed commit.
Every git call goes through the injected `git(args, cwd)`.
"""
import os
import shutil
import time

from . import runtime_files

# Local commits the planners may leave; anything else is foreign. Runtime
# files (runtime_files.py) are neither: they are stripped, never replayed.
PLAN_ONLY = ("planning/plans/", "brain/")
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
    return all(f.startswith(PLAN_ONLY) or runtime_files.is_runtime(f)
               for f in files)


def foreign(files):
    """Files outside plans that are not runtime files either."""
    return [f for f in files if f and not f.startswith(PLAN_ONLY)
            and not runtime_files.is_runtime(f)]


def commit_files(git, root, sha):
    r = git(["show", "--name-only", "--format=", sha], root)
    return [f for f in r.stdout.splitlines() if f]


def local_commits(git, root):
    """Oldest first: the commits on HEAD that origin/main lacks."""
    return git(["rev-list", "--reverse", "origin/main..HEAD"],
               root).stdout.split()


def _runner(git, root):
    def run(args):
        r = git(args[1:] if args[:1] == ["git"] else args, root)
        if r.returncode:
            raise RuntimeError(f"git {' '.join(args[1:3])}: "
                               f"{r.stderr.strip()[:150]}")
        return r.stdout
    return run


def _pick_one(git, root, sha):
    """True when sha's changes, runtime files taken out, became a commit.
    Raises RuntimeError when it does not apply."""
    r = git(IDENTITY + ["cherry-pick", "-n", "-X", "ours", sha], root)
    if r.returncode:
        raise RuntimeError(r.stderr.strip()[:150])
    run = _runner(git, root)
    runtime_files.unstage(run)
    if not git(["diff", "--cached", "--quiet"], root).returncode:
        return False   # only runtime files, or already on main
    run(IDENTITY + ["commit", "-q", "--no-verify", "-C", sha])
    return True


def replay(git, root, commits):
    """Cherry-picks each commit without its runtime files (2026-11-14: a
    replayed plan commit must never track state.json again), main's side
    winning conflicting hunks as in the rebase; one that still does not
    apply, is already on main or carried only runtime files is skipped: it
    is on the keep branch. Returns (applied, skipped)."""
    applied = skipped = 0
    for sha in commits:
        try:
            ok = _pick_one(git, root, sha)
        except RuntimeError:
            ok = False
        if ok:
            applied += 1
            continue
        git(["cherry-pick", "--quit"], root)
        git(["reset", "-q", "--hard", "HEAD"], root)
        skipped += 1
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

"""Which head of a task's branch counts, and catching the local one up.

Agents' sub-branches are worktrees of the daemon's repository, so the local
ref is usually the truth (an unpushed hand fix must count). But a stall
resolver or the coordinator works in a worktree of its own and can only
push: 2026-11-09, plugins.offline.ui was finished by hand on
origin/sub/plugins.offline/ui (7b6fcab4) while the local branch stayed at
the blocked a127a25e. triage.hand_fixed read the local ref only, never saw
the fix, and the feature's contract and engine sat in `integrating` for 4 h
waiting on the blocked sibling. The newer of the two heads now counts, and
a pushed fix is fast-forwarded into the local branch so the tester and the
integrator, who work from the local branch, see it.

`run(argv, cwd, timeout)` is forge.run's signature (a CompletedProcess)."""

FETCH_TIMEOUT_S = 120


def _tip(run, root, ref):
    r = run(["git", "log", "-1", "--format=%H%x1f%ct%x1f%s", ref, "--"],
            root, 60)
    if r.returncode:
        return None
    parts = r.stdout.strip().split("\x1f")
    if len(parts) != 3 or not parts[1].isdigit():
        return None
    return {"sha": parts[0], "ts": float(parts[1]), "subject": parts[2]}


def _is_ancestor(run, root, a, b):
    return run(["git", "merge-base", "--is-ancestor", a, b],
               root, 60).returncode == 0


def fetch(run, root, branch):
    """Refresh origin/<branch>; a branch not on origin (or no network) is
    not an error, the local ref still answers."""
    try:
        run(["git", "fetch", "-q", "origin",
             f"+refs/heads/{branch}:refs/remotes/origin/{branch}"],
            root, FETCH_TIMEOUT_S)
    except Exception:  # noqa: BLE001 - a fetch timeout leaves the old ref
        pass


def branch_tip(run, root, branch):
    """{sha, ts, subject, ref} of the head that counts, ref "local" or
    "origin"; None when neither exists. origin wins only when it is strictly
    ahead of the local branch: a local branch that is ahead or diverged holds
    work nobody pushed, and that is what the agent left."""
    fetch(run, root, branch)
    local = _tip(run, root, f"refs/heads/{branch}")
    remote = _tip(run, root, f"refs/remotes/origin/{branch}")
    if local:
        local["ref"] = "local"
    if remote:
        remote["ref"] = "origin"
    if not local or not remote:
        return local or remote
    if remote["sha"] != local["sha"] and \
            _is_ancestor(run, root, local["sha"], remote["sha"]):
        return remote
    return local


def _worktree_of(run, root, branch):
    r = run(["git", "worktree", "list", "--porcelain"], root, 60)
    if r.returncode:
        return None
    path = None
    for ln in r.stdout.splitlines():
        if ln.startswith("worktree "):
            path = ln[len("worktree "):]
        elif ln == f"branch refs/heads/{branch}":
            return path
    return None


def sync_local(run, root, branch):
    """Fast-forward the local branch to origin/<branch>. (ok, why).

    Only a fast-forward: a local branch with commits of its own is left
    alone. A worktree with the branch checked out is moved only when it is
    clean, so nothing an agent left is lost."""
    head = _tip(run, root, f"refs/remotes/origin/{branch}")
    if head is None:
        return False, f"origin/{branch} not found"
    local = _tip(run, root, f"refs/heads/{branch}")
    if local is None:
        r = run(["git", "branch", branch, head["sha"]], root, 60)
        return r.returncode == 0, (r.stderr or "").strip() or "created"
    if local["sha"] == head["sha"]:
        return True, "already at origin"
    if not _is_ancestor(run, root, local["sha"], head["sha"]):
        return False, f"local {branch} has commits not on origin"
    wt = _worktree_of(run, root, branch)
    if wt is None:
        r = run(["git", "update-ref", f"refs/heads/{branch}", head["sha"],
                 local["sha"]], root, 60)
        return r.returncode == 0, (r.stderr or "").strip() or "fast-forwarded"
    dirty = run(["git", "status", "--porcelain", "--untracked-files=no"],
                wt, 60)
    if dirty.returncode or dirty.stdout.strip():
        return False, f"worktree {wt} has uncommitted changes"
    r = run(["git", "merge", "-q", "--ff-only", head["sha"]], wt, 120)
    return r.returncode == 0, (r.stderr or "").strip() or "fast-forwarded"


def _resolve(run, root, *refs):
    for ref in refs:
        if run(["git", "rev-parse", "-q", "--verify", ref + "^{commit}"],
               root, 60).returncode == 0:
            return ref
    return None


def changed_files(run, root, base, branch):
    """Paths the branch changed since it left `base` (the task's start_from;
    origin/main when that branch was never made), from the head that counts.
    None when the branch is unknown. A scope triage widens the claim with
    these, not with the agent's shorthand for them."""
    tip = branch_tip(run, root, branch)
    if tip is None:
        return None
    head = ("refs/remotes/origin/" if tip["ref"] == "origin"
            else "refs/heads/") + branch
    start = _resolve(run, root, *(
        [f"refs/heads/{base}", f"refs/remotes/origin/{base}"] if base else []),
        "refs/remotes/origin/main", "refs/heads/main")
    if start is None:
        return None
    r = run(["git", "diff", "--name-only", f"{start}...{head}", "--"],
            root, 120)
    if r.returncode:
        return None
    return [ln for ln in r.stdout.splitlines() if ln.strip()]

"""Pushing a feature branch the daemon owns (race with a stray pusher).

The daemon rebases the feature branch in its own worktree, so a copy that
someone else already pushed (an integrator that ignored its brief, 2026-11-07,
#500-#503) is rejected as non-fast-forward. Two cases: the remote carries the
same patch against main (adopt it, push nothing), or it differs. Then the
remote's commits are merged into the local branch first and the push is a
plain fast-forward: never --force, never --force-with-lease (2026-11-08,
#574/#579 lost commits others pushed meanwhile). A push rejected because the
remote moved again is retried after fetch+merge.
"""
import hashlib
import re

SAME, PUSHED = "same", "pushed"


def _normalised_patch(diff):
    """Hash of a diff that survives a rebase: blob ids and line offsets
    change when the base moves, the changed lines do not."""
    keep = []
    for line in diff.splitlines():
        if line.startswith("index "):
            continue
        keep.append(re.sub(r"^(@@ )[-+0-9, ]+( @@)", r"\1\2", line))
    return hashlib.sha1("\n".join(keep).encode("utf-8")).hexdigest()


def _remote_head(ok, worktree, branch):
    """Sha of origin/<branch>, or None when the branch is not on origin."""
    out = ok(["git", "ls-remote", "--heads", "origin", branch], worktree, 60)
    first = out.split()[:1]
    return first[0] if first else None


def _patch_of(ok, worktree, base_ref, rev):
    return _normalised_patch(
        ok(["git", "diff", "--no-color", f"{base_ref}...{rev}"], worktree, 120))


def _contains(ok, worktree, ancestor, descendant):
    try:
        ok(["git", "merge-base", "--is-ancestor", ancestor, descendant],
           worktree, 60)
        return True
    except Exception:  # exit 1 = not an ancestor
        return False


def _merge_remote(ok, worktree, branch):
    """Bring FETCH_HEAD into the checked-out branch; a conflict aborts the
    merge and raises, so nothing is pushed over the remote's work."""
    if _contains(ok, worktree, "FETCH_HEAD", "HEAD"):
        return
    try:
        ok(["git", "merge", "--no-edit", "FETCH_HEAD"], worktree, 300)
    except Exception:
        try:
            ok(["git", "merge", "--abort"], worktree, 60)
        except Exception:
            pass
        raise


def push_feature_branch(ok, worktree, branch, base_ref="origin/main",
                        attempts=3):
    """`ok(argv, cwd, timeout)` runs a command and returns stdout, raising
    on failure. Returns SAME when origin already holds this patch. The
    worktree has `branch` checked out."""
    for attempt in range(attempts):
        remote = _remote_head(ok, worktree, branch)
        if remote is None:
            ok(["git", "push", "origin", f"{branch}:{branch}"], worktree, 300)
            return PUSHED
        ok(["git", "fetch", "origin", branch], worktree, 300)
        if _patch_of(ok, worktree, base_ref, "FETCH_HEAD") ==                 _patch_of(ok, worktree, base_ref, branch):
            return SAME
        _merge_remote(ok, worktree, branch)
        try:
            ok(["git", "push", "origin", f"{branch}:{branch}"], worktree, 300)
            return PUSHED
        except Exception:
            if attempt == attempts - 1:
                raise
    return PUSHED

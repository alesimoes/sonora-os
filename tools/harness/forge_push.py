"""Pushing a feature branch the daemon owns (race with a stray pusher).

The daemon rebases the feature branch in its own worktree, so a copy that
someone else already pushed (an integrator that ignored its brief, 2026-11-07,
#500-#503) is rejected as non-fast-forward. Two cases: the remote carries the
same patch against main (adopt it, push nothing), or it differs and the
daemon's branch wins, replacing only the head it saw (--force-with-lease).
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


def push_feature_branch(ok, worktree, branch, base_ref="origin/main"):
    """`ok(argv, cwd, timeout)` runs a command and returns stdout, raising
    on failure. Returns SAME when origin already holds this patch."""
    remote = _remote_head(ok, worktree, branch)
    if remote is None:
        ok(["git", "push", "origin", f"{branch}:{branch}"], worktree, 300)
        return PUSHED
    ok(["git", "fetch", "origin", branch], worktree, 300)
    if _patch_of(ok, worktree, base_ref, "FETCH_HEAD") == \
            _patch_of(ok, worktree, base_ref, branch):
        return SAME
    ok(["git", "push", f"--force-with-lease={branch}:{remote}", "origin",
        f"{branch}:{branch}"], worktree, 300)
    return PUSHED

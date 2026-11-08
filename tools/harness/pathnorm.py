"""Claim-path candidates for a hook's absolute file path.

Agents claim repo-relative paths ("tools/x"), but the PreToolUse hook gets an
absolute one (Windows "C:\\..." or posix). A fixer works in a worktree nested
inside the daemon's worktree, so the same file is relative to two roots; the
agent may have claimed either form. The hook asks the broker about each.
"""
import os


def _posix(p):
    return p.replace("\\", "/")


def candidates(path, exists=os.path.lexists):
    """Repo-relative forms of `path`, innermost worktree first, then the
    plain posix path. Roots are ancestors holding a `.git` (dir or file)."""
    path = _posix(path)
    out = []
    parts = path.split("/")
    for i in range(len(parts) - 1, 0, -1):
        root = "/".join(parts[:i])
        if root and exists(root + "/.git"):
            rel = "/".join(parts[i:])
            if rel and rel not in out:
                out.append(rel)
    if path not in out:
        out.append(path)
    return out

"""PreToolUse guard for Bash, beside gitguard: an agent working in the
daemon's checkout (the planners: architect, lead) must not commit the
daemon's runtime files (2026-11-14: a plan commit there tracked state.json,
events.jsonl and .harness/plan/*; the forced self-update dropped it and git
deleted the daemon's store).

In the daemon checkout it refuses
  - `git add -f/--force` (the runtime files are .gitignore'd; forcing is
    the only way a plan commit picks them up),
  - `git add` naming a runtime path,
  - a broad `git add` (-A, -u, ., :/) while HEAD tracks a runtime file,
  - `git commit` while the index stages one (with -a: or the tree changes
    one).
Plans are committed with explicit paths: `git add planning/plans/x.md`.
"""
import subprocess

try:
    from . import gitguard, runtime_files
except ImportError:   # loaded by tools/harness.py with harness/ on sys.path
    import gitguard
    import runtime_files

MESSAGE = ("the daemon's runtime files (state.json, events*.jsonl, "
           "status.json, .harness/) must never be committed; add plans by "
           "explicit path, without -f")
BROAD = frozenset({"-A", "--all", "-u", "--update", ".", ":/", "*",
                   "--no-ignore-removal"})


def _git(where, args):
    """stdout of a read-only git query, or None when git cannot answer."""
    try:
        r = subprocess.run(["git", "-C", where] + list(args),
                           capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def _names(out):
    return [f for f in (out or "").splitlines() if f]


def _short(args, letter):
    return any(a.startswith("-") and not a.startswith("--") and
               letter in a[1:] for a in args)


def _rel(where, arg, root):
    p = gitguard._norm(gitguard._join(where, arg))
    r = gitguard._norm(root)
    return p[len(r) + 1:] if p.startswith(r + "/") else None


def _tracked_runtime(where, git):
    return runtime_files.runtime_only(
        _names(git(where, ["ls-tree", "-r", "--name-only", "HEAD"])))


def add_reason(where, args, root, git=_git):
    if "--force" in args or _short(args, "f"):
        return "git add -f/--force in the daemon checkout"
    paths = [a for a in args if not a.startswith("-") or a in BROAD]
    named = [p for p in paths
             if runtime_files.is_runtime(_rel(where, p, root) or "")]
    if named:
        return "git add of runtime files: " + " ".join(named)[:150]
    if any(a in BROAD for a in args) or _short(args, "A") or \
            _short(args, "u"):
        held = _tracked_runtime(where, git)
        if held:
            return ("HEAD tracks runtime files (" + " ".join(held)[:120]
                    + "); add plans by explicit path")
    return None


def commit_reason(where, args, git=_git):
    staged = _names(git(where, ["diff", "--cached", "--name-only"]))
    if "--all" in args or _short(args, "a"):
        staged += _names(git(where, ["diff", "--name-only"]))
    bad = runtime_files.runtime_only(staged)
    if bad:
        return "the commit would carry runtime files: " + " ".join(bad)[:150]
    return None


def reason(command, cwd, root, git=_git):
    """Why `command` is refused, or None."""
    for where, sub, rest in gitguard.segments(command, cwd):
        if not gitguard.is_daemon_dir(where, root):
            continue
        why = add_reason(where, rest, root, git) if sub == "add" else \
            commit_reason(where, rest, git) if sub == "commit" else None
        if why:
            return why
    return None

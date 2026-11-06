"""PreToolUse guard for Bash: an agent must not move the daemon's own checkout
(an agent once ran `git switch` there and broke a batch). Refuses git
checkout|switch|reset|clean|worktree remove aimed at the daemon root, by cwd,
by `cd`, or by `git -C`. Worktrees live under <root>/.claude/worktrees, so
only the root and its ordinary subdirectories count, not those."""
import os
import posixpath
import re
import shlex

MESSAGE = "the daemon's checkout is off-limits; work in your own worktree"
MOVERS = ("checkout", "switch", "reset", "clean")
SEPARATORS = re.compile(r"&&|\|\||;|\||\n")
SKIP_UNDER = (".claude/worktrees", ".harness")


def _drive(path):
    """Git bash writes C:\\x as /c/x."""
    m = re.match(r"^/([a-zA-Z])(?:/(.*))?$", path.replace("\\", "/"))
    return f"{m.group(1)}:/{m.group(2) or ''}" if m else path


def _isabs(p):
    """Windows drive paths count as absolute on every host, so the guard
    behaves the same under Git bash and on a Linux runner."""
    return bool(re.match(r"^[a-zA-Z]:/", p)) or p.startswith("/")


def _norm(path):
    p = _drive(path).replace("\\", "/")
    if not _isabs(p):
        p = os.path.abspath(p).replace("\\", "/")
    p = posixpath.normpath(p)
    if re.match(r"^[a-zA-Z]:/", p):
        p = p.lower()
    return p.rstrip("/")


def _join(here, path):
    path = _drive(os.path.expanduser(path)).replace("\\", "/")
    return path if _isabs(path) else here.replace("\\", "/") + "/" + path


def is_daemon_dir(path, root):
    p, r = _norm(path), _norm(root)
    if p == r:
        return True
    if not p.startswith(r + "/"):
        return False
    rel = p[len(r) + 1:]
    return not any(rel == s or rel.startswith(s + "/") for s in SKIP_UNDER)


def _words(segment):
    try:
        return shlex.split(segment.replace("\\", "/"), posix=True)
    except ValueError:
        return segment.split()


def _moves_tree(words):
    """(git target dir from -C or None, True) when this is a mover command."""
    if "git" not in words:
        return None, False
    i = words.index("git") + 1
    target = None
    while i < len(words) and words[i].startswith("-"):
        if words[i] == "-C" and i + 1 < len(words):
            target = words[i + 1]
            i += 1
        elif words[i] in ("-c", "--git-dir", "--work-tree"):
            i += 1
        i += 1
    rest = words[i:]
    if not rest:
        return target, False
    sub = rest[0]
    if sub in MOVERS:
        return target, True
    return target, sub == "worktree" and "remove" in rest[1:]


def violates(command, cwd, root):
    """True when some git mover in `command` would run in the daemon root."""
    here = cwd or os.getcwd()
    for seg in SEPARATORS.split(command or ""):
        words = _words(seg.strip())
        if not words:
            continue
        if words[0] == "cd" and len(words) > 1:
            here = _join(here, words[1])
            continue
        target, mover = _moves_tree(words)
        if mover:
            where = _join(here, target) if target else here
            if is_daemon_dir(where, root):
                return True
    return False

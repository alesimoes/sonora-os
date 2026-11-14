"""The daemon's runtime files: what it writes into its own checkout and git
must never carry (2026-11-14: a plan commit in the daemon checkout tracked
state.json and events.jsonl; the forced self-update then dropped that
commit and git deleted both, so the daemon came back on an empty store).

One list, mirrored by .gitignore, used by every harness path that stages
(stage_all), replays (unstage) or moves the checkout, and by the agents'
Bash guard (commitguard.py). Every git call goes through an injected
`run(args) -> stdout` that raises on failure.
"""
import fnmatch

ODIR = "planning/orchestrator/"
# Whole trees that are runtime output.
DIRS = (".harness/", ODIR + "plans/")
# Single files under planning/orchestrator (fnmatch patterns).
FILES = tuple(ODIR + p for p in (
    "state.json", "state.json.bak.*", "state.quarantine.json", "state.db*",
    "events*.jsonl", "status.json", "STOP*", "paused.json",
    "waits-on-owner.md", "shadow.log"))
# Kept across every move of the checkout even when git does not track them.
CORE = tuple(ODIR + p for p in ("state.json", "events.jsonl", "status.json",
                                "paused.json", "waits-on-owner.md"))
EXCLUDES = tuple(":(exclude)" + p.rstrip("/") for p in DIRS + FILES)


def norm(path):
    p = path.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def is_runtime(path):
    p = norm(path)
    if any(p == d.rstrip("/") or p.startswith(d) for d in DIRS):
        return True
    return any(fnmatch.fnmatchcase(p, pat) for pat in FILES)


def runtime_only(paths):
    return [p for p in paths if is_runtime(p)]


def _names(out):
    return [f for f in (out or "").replace("\0", "\n").splitlines() if f]


def add_all_argv():
    """`git add -A` over the whole tree except the runtime files."""
    return ["git", "add", "-A", "--", "."] + list(EXCLUDES)


def staged(run):
    """Runtime files the index stages against HEAD."""
    return runtime_only(_names(run(["git", "diff", "--cached",
                                    "--name-only", "-z"])))


def tracked(run, ref="HEAD"):
    """Runtime files `ref` tracks (the result of an earlier bad commit)."""
    return runtime_only(_names(run(["git", "ls-tree", "-r", "-z",
                                    "--name-only", ref])))


def unstage(run):
    """Takes runtime files out of the index (the files stay on disk);
    raises when some are still staged afterwards. Returns what it unstaged."""
    bad = staged(run)
    if bad:
        run(["git", "reset", "-q", "--"] + bad)
        left = staged(run)
        if left:
            raise RuntimeError("runtime files still staged: "
                               + " ".join(left)[:200])
    return bad


def stage_all(run):
    """Stages every change except runtime files, by explicit pathspec; a
    runtime file that is staged anyway (tracked by HEAD) is unstaged."""
    run(add_all_argv())
    return unstage(run)

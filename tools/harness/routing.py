"""Reads .claude/routing.toml (owner process, 2026-11-06) at run time.

The file is optional: without it (or without tomllib) every lookup answers
None and the caller keeps today's behaviour. Nothing here launches anything.
"""
import os
import re

try:
    import tomllib
except ImportError:     # Python < 3.11
    tomllib = None

ROUTING_PATH = os.path.join(".claude", "routing.toml")
# What plans.TRACK_ENGINEERS held before routing.toml existed. The plan
# schema's `track` enum is these three, so [tracks] only refines them.
DEFAULT_TRACKS = {
    "daw": ("rt-engineer", "ui-engineer"),
    "plugin": ("dsp-engineer", "plugin-engineer"),
    "os": ("distro-engineer", "driver-engineer"),
}


def glob_regex(pattern):
    """`**` crosses directories, `*` and `?` do not."""
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            i += 2
            if pattern.startswith("/", i):
                i += 1      # "a/**/b" also matches "a/b"
                out.append("(?:.*/)?")
            else:
                out.append(".*")
            continue
        c = pattern[i]
        out.append("[^/]*" if c == "*" else "[^/]" if c == "?"
                   else re.escape(c))
        i += 1
    return re.compile("".join(out) + r"\Z")


class Routing:
    def __init__(self, data):
        self.data = data
        self.paths = [(glob_regex(g), a) for g, a in
                      (data.get("paths") or {}).items()]

    # -- paths ----------------------------------------------------------
    def agent_for_path(self, path):
        path = path.replace("\\", "/")
        if path.startswith("./"):
            path = path[2:]
        for rx, agent in self.paths:     # TOML keeps file order: first wins
            if rx.match(path):
                return agent
        return None

    def agent_for_files(self, files):
        """The agent most files belong to; a tie goes to the first file."""
        counts, first = {}, {}
        for n, f in enumerate(files):
            a = self.agent_for_path(f)
            if a:
                counts[a] = counts.get(a, 0) + 1
                first.setdefault(a, n)
        if not counts:
            return None
        return max(counts, key=lambda a: (counts[a], -first[a]))

    # -- failures -------------------------------------------------------
    def agent_for_failure(self, kind, files=()):
        """`owner-of-path` is resolved through [paths]."""
        agent = (self.data.get("failures") or {}).get(kind)
        if agent == "owner-of-path":
            return self.agent_for_files(files)
        return agent if isinstance(agent, str) else None

    def stalled_after_hours(self, default=2.0):
        v = (self.data.get("failures") or {}).get("stalled_after_hours")
        return float(v) if isinstance(v, (int, float)) else default

    # -- tracks / tiers / planning ---------------------------------------
    def tracks(self):
        raw = self.data.get("tracks") or {}
        return {k: tuple(v) for k, v in raw.items()
                if isinstance(v, list) and v}

    def tiers(self):
        return self.data.get("tiers") or {}

    def planning(self):
        return self.data.get("planning") or {}


def load(root):
    """Routing for the repo at `root`, or None (missing, unreadable, or no
    tomllib)."""
    if tomllib is None:
        return None
    try:
        with open(os.path.join(root, ROUTING_PATH), "rb") as f:
            return Routing(tomllib.load(f))
    except (OSError, ValueError):
        return None


def track_engineers(routing):
    """{track: agents}. routing.toml's [tracks] adds specialists (designer,
    tester) to the three built-in tracks; the old engineers stay valid, so a
    plan written before the file still validates."""
    out = dict(DEFAULT_TRACKS)
    if routing:
        for k, v in routing.tracks().items():
            if k in out:
                out[k] = tuple(dict.fromkeys(v + out[k]))
    return out

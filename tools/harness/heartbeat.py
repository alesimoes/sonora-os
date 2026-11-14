"""Liveness of a run (ADR-0047 section 3). The agent does not have to
cooperate: process, stream log, git and file activity decide."""
import hashlib
import json
import os
import re
import subprocess

from . import runtime_files

RESULT_STATUS = ("done", "fail", "blocked")


def read_result(path):
    """(data, error). A missing or invalid file is not a result."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return None, "missing"
    except (OSError, ValueError) as e:
        return None, f"invalid json: {e}"
    if not isinstance(data, dict):
        return None, "not an object"
    if data.get("status") not in RESULT_STATUS:
        return None, "bad status"
    if not isinstance(data.get("summary", ""), str):
        return None, "bad summary"
    for k in ("findings", "followups", "unverified"):
        # Agents often write one sentence here; losing a finished run over
        # the shape of a note costs a whole relaunch.
        if isinstance(data.get(k), str):
            data[k] = [data[k]] if data[k].strip() else []
        if not isinstance(data.get(k, []), list):
            return None, f"{k} must be a list"
    return data, None


def _git(worktree, *args):
    try:
        r = subprocess.run(["git", "-C", worktree, *args],
                           capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def _git_or_raise(worktree, argv):
    """argv is ["git", ...]; stdout, or RuntimeError on a failed call."""
    out = _git(worktree, *argv[1:])
    if out is None:
        raise RuntimeError(f"{' '.join(argv[:3])} failed in {worktree}")
    return out


def _mtime(path):
    try:
        return os.stat(path).st_mtime
    except OSError:
        return 0.0


class Activity:
    """Remembers, per run, a signature of the worktree; a change is a beat
    stamped with `now`. Stream log mtime counts directly."""

    def __init__(self):
        self.sig = {}

    def _signature(self, worktree):
        # One git call: the branch.oid header is HEAD, the rest are changes.
        out = None
        if os.path.exists(os.path.join(worktree, ".git")):
            out = _git(worktree, "status", "--porcelain=v2", "--branch")
        if out is None:
            files = []
            n = 0
            for dp, dn, fn in os.walk(worktree):
                dn[:] = [d for d in dn if d != ".git"]
                for f in fn:
                    p = os.path.join(dp, f)
                    files.append((p, _mtime(p)))
                    n += 1
                if n > 2000:
                    break
            return hashlib.sha1(repr(sorted(files)).encode()).hexdigest()
        stamps = []
        for line in out.splitlines():
            if line.startswith("# branch.oid"):
                stamps.append((line, 0))
            elif not line.startswith("#"):
                path = line.split(" ", 8)[-1] if line[0] in "12" else line[2:]
                stamps.append((line, _mtime(os.path.join(
                    worktree, path.split("\t")[0].strip('"')))))
        return hashlib.sha1(repr(stamps).encode()).hexdigest()

    def sample(self, run_id, worktree, stream_path, last_beat, now):
        beat = max(last_beat or 0, _mtime(stream_path))
        if worktree and os.path.isdir(worktree):
            sig = self._signature(worktree)
            if self.sig.get(run_id) not in (None, sig):
                beat = max(beat, now)
            self.sig[run_id] = sig
        return beat

    def forget(self, run_id):
        self.sig.pop(run_id, None)


def limit_for(role, building, cfg):
    if role == "tester":
        return cfg.tester_s
    if building:
        return cfg.build_s
    return cfg.stalled_s


def classify(*, alive, result_ok, now, started, last_beat, role, building, cfg):
    """finished | dead | ceiling | stalled | suspect | ok"""
    if result_ok:
        return "finished"
    if not alive:
        return "dead"
    if now - started > cfg.ceiling_s:
        return "ceiling"
    age = now - (last_beat or started)
    if age > limit_for(role, building, cfg):
        return "stalled"
    if age > cfg.suspect_s:
        return "suspect"
    return "ok"


def is_working(last_beat, building, now, cfg):
    """'Building is not working': a run counts toward a floor only while it
    beats, or while it holds a build slot (the build is the work)."""
    return building or (now - (last_beat or 0)) < cfg.working_s


def wip_checkpoint(worktree):
    """Commit uncommitted work as a WIP checkpoint. Never pushes."""
    if not worktree or not os.path.isdir(worktree):
        return False
    st = _git(worktree, "status", "--porcelain")
    if not st or not st.strip():
        return False
    try:
        runtime_files.stage_all(lambda a: _git_or_raise(worktree, a))
    except (OSError, RuntimeError, subprocess.SubprocessError):
        return False
    r = subprocess.run(
        ["git", "-C", worktree, "-c", "user.name=orchestrator",
         "-c", "user.email=orchestrator@localhost", "commit", "-q", "-m",
         "WIP: orchestrator checkpoint"], capture_output=True, text=True)
    return r.returncode == 0


_LIMIT = re.compile(r"usage limit|rate.?limit|\b429\b|overloaded_error"
                    r"|too many requests", re.I)
_RESET = re.compile(r"(?:reset|retry)\w*\s+(?:in|after)\s+(\d+)\s*"
                    r"(second|sec|s|minute|min|m|hour|hr|h)", re.I)


def usage_limit(stream_path, default_s, tail_bytes=8192):
    """None, or the pause in seconds when the run's output tail shows a
    usage-limit exit."""
    try:
        with open(stream_path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - tail_bytes))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return None
    if not _LIMIT.search(tail):
        return None
    m = _RESET.search(tail)
    if not m:
        return default_s
    n, unit = int(m.group(1)), m.group(2).lower()
    return n * (3600 if unit.startswith("h") else
                60 if unit.startswith("m") else 1)

"""What status.json says about one running agent in plain words: the task's
title (first meaningful line of its brief), its kind, branch and PR."""
import re

TITLE_MAX = 200
_MARKUP = re.compile(r"^[#>*\-\s`]+")


def brief_title(brief, fallback=""):
    """First non-empty line of the brief with markdown bullets stripped."""
    for line in str(brief or "").splitlines():
        text = _MARKUP.sub("", line).strip()
        if text:
            return text[:TITLE_MAX]
    return fallback


def task_title(t):
    """A wake task's brief is an event line, so name the feature it is about;
    never the raw wake-... id."""
    fid = t.get("feature_id")
    if t.get("kind") == "wake" and fid:
        return fid
    return brief_title(t.get("brief"), t.get("feature_id") or t.get("id") or "")


def launched_by(task):
    """Who started the task: wake tasks are the daemon's own; the rest were
    queued by the coordinator."""
    tid = str((task or {}).get("id") or "")
    return "daemon" if tid.startswith("wake-") else "coordenador"


def run_view(run, task):
    """Extra status fields for a run; `task` may be missing (then {})."""
    t = task or {}
    return {"title": task_title(t),
            "kind": t.get("kind"), "agent": t.get("agent"),
            "branch": t.get("branch"), "pr": t.get("pr"),
            "stream_path": run.get("stream_path"),
            "model": run.get("model"), "launched_by": launched_by(task)}

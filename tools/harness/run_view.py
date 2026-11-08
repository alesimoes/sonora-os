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


def run_view(run, task):
    """Extra status fields for a run; `task` may be missing (then {})."""
    t = task or {}
    return {"title": brief_title(t.get("brief"), t.get("id") or ""),
            "kind": t.get("kind"), "agent": t.get("agent"),
            "branch": t.get("branch"), "pr": t.get("pr"),
            "feature": t.get("feature_id"),
            "stream_path": run.get("stream_path")}

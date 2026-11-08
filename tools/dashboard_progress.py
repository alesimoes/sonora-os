"""Reads main's progress catalogue (planning/progress.json on origin/main),
the same source the old dashboard used; only merged and verified work counts,
as in planning/PROGRESS.md. Returns None when it cannot be read."""
import json

from dashboard_kpis import progress_view
from dashboard_util import orch_root, run

EVERY_S = 300


def read_progress():
    root = orch_root()
    run(["git", "-C", root, "fetch", "-q", "origin", "main"], timeout=60)
    raw = run(["git", "-C", root, "show", "origin/main:planning/progress.json"])
    return progress_view(json.loads(raw)) if raw else None

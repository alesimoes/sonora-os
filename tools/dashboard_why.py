"""Attaches 'Por que não entrou?' (harness/whynot.py) to each open PR the
dashboard shows. Reads only status.json's content (already loaded) and the
tail of the daemon's events.jsonl."""
import json
import os

from dashboard_daemon import daemon_alive
from dashboard_util import orch_status_path
from harness import whynot

TAIL_BYTES = 400_000


def recent_events(path=None):
    """Newest first; only the file's tail is read."""
    path = path or os.path.join(os.path.dirname(orch_status_path()), "events.jsonl")
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - TAIL_BYTES))
            raw = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []
    out = []
    for ln in reversed(raw):
        try:
            e = json.loads(ln)
        except ValueError:      # the cut first line or a torn last one
            continue
        if isinstance(e, dict):
            out.append(e)
    return out


def with_why(prs, status, events=None, up=None):
    """Copies of the PR rows, each with a `why` dict; one bad row never hides the rest."""
    events = recent_events() if events is None else events
    up = daemon_alive() if up is None else up
    out = []
    for p in prs:
        try:
            why = whynot.explain(p, status if isinstance(status, dict) else {}, events, up)
        except Exception as e:
            why = {"reason": "não consegui explicar: %s" % e}
        out.append(dict(p, why=why))
    return out

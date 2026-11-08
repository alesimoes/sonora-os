"""The console's numbers at a glance (owner, 2026-11-08): how far along the
project is, the open PRs with what the train does with each, and the merges
of today and of the last hour. Pure functions of data already read, so every
rule is unit-tested; the readers are dashboard_progress.py and
dashboard_github.py."""
import time
from datetime import datetime

import progress as catalogue

# Plugins are their own track on the console; they are modules of the DAW
# front in the catalogue, so they leave "Complete DAW" and the three tracks
# add up to the overall figure.
PLUGIN_MODULES = ("daw.plugins", "daw.native", "daw.ampcab")
TRACKS = (("daw", "Complete DAW"), ("plugins", "Plugins"), ("os", "OS packaging"))
GATE_LEVEL = {"verde": "ok", "vermelho": "bad", "conflito": "bad", "cancelado": "bad",
              "rodando": "run"}
GATE_WORD = {"verde": "verde", "vermelho": "vermelho", "conflito": "vermelho",
             "cancelado": "vermelho", "rodando": "rodando"}
MAX_PRS = 30
HOUR_S = 3600


def _one(t):
    return {"pct": t.pct(), "done": t.done, "doing": t.doing, "total": t.total}


def _sum(tallies):
    out = catalogue.Tally()
    for t in tallies:
        out.merge(t)
    return out


def progress_view(data):
    """Overall and per-track percent and points from the catalogue dict."""
    overall, fronts, _ = catalogue.tally(data)
    by_track = {"daw": [], "plugins": [], "os": []}
    for front, _, modules in fronts:
        for module, t in modules:
            if module["id"] in PLUGIN_MODULES:
                by_track["plugins"].append(t)
            else:
                by_track["daw" if front["id"] == "daw" else "os"].append(t)
    tracks = [dict(_one(_sum(by_track[k])), key=k, title=title) for k, title in TRACKS]
    return dict(_one(overall), tracks=tracks)


def _train_text(row, train):
    """What the train is doing with the PR, in words."""
    t = train or {}
    bits = [t.get("phase") or t.get("state"), row.get("owner")]
    seen = []
    for b in bits:
        if b and str(b) not in seen:
            seen.append(str(b))
    return " · ".join(seen)


def pr_list(board_prs, train_rows, now=None):
    """One row per open PR, newest last; `tap` is the question the chat gets."""
    train = {t.get("pr"): t for t in train_rows or []}
    out = []
    for p in (board_prs or [])[:MAX_PRS]:
        gate = p.get("gate")
        out.append({"number": p["number"], "title": p.get("title") or "",
                    "ci": GATE_WORD.get(gate, gate or "rodando"),
                    "level": GATE_LEVEL.get(gate, "warn"), "ci_text": p.get("gate_text") or "",
                    "train": _train_text(p, train.get(p["number"])),
                    "minutes_open": p.get("opened_min"), "late": bool(p.get("late")),
                    "tap": f"por que o PR {p['number']} não entrou?"})
    return out


def _ts(iso):
    try:
        return datetime.fromisoformat(str(iso).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def merges_view(merged, now=None):
    """`merged` is today's merges (local day) from GitHub; the last hour is
    counted from their merge times."""
    now = now or time.time()
    stamps = [s for s in (_ts(m.get("mergedAt")) for m in merged or []) if s is not None]
    return {"today": len(stamps), "last_hour": sum(1 for s in stamps if now - s <= HOUR_S)}


def build(snap, now=None):
    """The `kpis` block of GET /api/console."""
    now = now or time.time()
    snap = snap or {}
    orch = snap.get("orchestrator") if isinstance(snap.get("orchestrator"), dict) else {}
    board = snap.get("board") or {}
    return {"progress": snap.get("progress"),
            "prs": pr_list(board.get("prs"), orch.get("train"), now),
            "merges": merges_view(snap.get("merged"), now)}

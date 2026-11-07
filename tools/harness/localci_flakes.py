"""Flaky tests the local gate caught (2026-11-07: about half of 24 gate runs
were red, each on a different load-sensitive test).

tools/local-ci.sh runs a red step once more before it counts; when the rerun
is green it appends `step=S tests=A,B sha=HEAD` to the run's `flakes` file.
The step passes, so the batch merges instead of being bisected, but the
flake is not forgotten: each one becomes a `flaky_test` event and a row of
the meta list `localci-flakes`, which the dashboard shows and
`orchestrator.py flakes` turns into BACKLOG lines (the coordinator's chore PR
adds them; the daemon never edits BACKLOG.md).
"""
import json
import os
import time

META = "localci-flakes"
KEEP = 200


def parse(text):
    """[{'step', 'tests': [...], 'sha'}] from a flakes file."""
    out = []
    for line in text.splitlines():
        f = dict(p.split("=", 1) for p in line.split() if "=" in p)
        if not f.get("step"):
            continue
        tests = [t for t in (f.get("tests") or "").split(",") if t]
        out.append({"step": f["step"], "tests": tests or ["unknown"],
                    "sha": f.get("sha", "")})
    return out


def read(logdir):
    try:
        with open(os.path.join(logdir, "flakes"), encoding="utf-8",
                  errors="replace") as f:
            return parse(f.read())
    except OSError:
        return []


def _rows(raw):
    try:
        rows = json.loads(raw or "[]")
    except ValueError:
        return []
    return rows if isinstance(rows, list) else []


def record(store, logdir, heads, now):
    """Events and meta rows for the flakes of a finished run; the count."""
    seen = read(logdir)
    if not seen:
        return 0
    rows = _rows(store.get_meta(META))
    for fl in seen:
        for t in fl["tests"]:
            rows.append({"test": t, "step": fl["step"], "at": now,
                         "heads": [h[:12] for h in heads]})
            store.event("local-ci", "flaky_test",
                        f"{t} in {fl['step']} (green on rerun; "
                        f"{','.join(h[:8] for h in heads)})")
    store.set_meta(META, json.dumps(rows[-KEEP:]))
    return len(seen)


def summary(rows):
    """[{'test', 'step', 'count', 'last'}], most frequent first."""
    by = {}
    for r in rows:
        k = (r.get("test"), r.get("step"))
        cur = by.setdefault(k, {"test": k[0], "step": k[1], "count": 0,
                                "last": 0})
        cur["count"] += 1
        cur["last"] = max(cur["last"], r.get("at") or 0)
    return sorted(by.values(), key=lambda x: (-x["count"], -x["last"]))


def from_store(store):
    return summary(_rows(store.get_meta(META)))


def backlog_lines(rows):
    """One BACKLOG line per flaky test, for the coordinator's chore PR."""
    out = []
    for s in summary(rows):
        day = time.strftime("%Y-%m-%d", time.localtime(s["last"] or 0))
        out.append(f"- [ ] Flaky under the local gate: `{s['test']}` in the "
                   f"{s['step']} step, green on rerun {s['count']}x (last "
                   f"{day}). Fix the test's cause; never skip it.")
    return out


def from_state_file(path):
    """Rows read straight from state.json, without opening a Store (a
    Store may rewrite the file; this is a read-only CLI)."""
    try:
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return []
    return _rows((doc.get("meta") or {}).get(META))

"""Reload-before-write for state.json (2026-11-07: CLI adoptions vanished).

The Store holds state.json in memory and writes it whole. When another
process wrote the file since this Store last read or wrote it (a CLI
command from before harness/daemonlock.py, a daemon of old code), the next
save would erase that write. Before saving, the Store compares the file's
signature with the one it left; when they differ it reads the file and keeps
what only the other writer added: rows of features, tasks and runs, meta
keys, and entries of meta values that are JSON objects (the train's
`train` map, where an adoption lives). What both sides have, this process
wins. One `state_external_write` event says how much was kept.
"""
import json
import os

SECTIONS = ("features", "tasks", "runs")
# Store._sig while it loads: a save then (quarantine) writes what it read.
LOADING = "loading"


def signature(path):
    try:
        st = os.stat(path)
    except OSError:
        return None
    return (st.st_mtime_ns, st.st_size)


def _json_obj(v):
    try:
        d = json.loads(v) if isinstance(v, str) else None
    except ValueError:
        return None
    return d if isinstance(d, dict) else None


def merge_into(mine, theirs):
    """Adds to `mine` what only `theirs` has; returns the count added."""
    n = 0
    for sec in SECTIONS:
        dst, src = mine.setdefault(sec, {}), theirs.get(sec) or {}
        for k, v in src.items():
            if k not in dst:
                dst[k] = v
                n += 1
    dst, src = mine.setdefault("meta", {}), theirs.get("meta") or {}
    for k, v in src.items():
        if k not in dst:
            dst[k] = v
            n += 1
            continue
        a, b = _json_obj(dst[k]), _json_obj(v)
        if a is None or b is None:
            continue
        extra = {kk: vv for kk, vv in b.items() if kk not in a}
        if extra:
            a.update(extra)
            dst[k] = json.dumps(a)
            n += len(extra)
    return n


def merge_external(store):
    """Called by Store._save: keeps another writer's additions."""
    mine = getattr(store, "_sig", None)
    sig = signature(store.path)
    if mine == LOADING or sig is None or sig == mine:
        return 0
    try:
        with open(store.path, encoding="utf-8") as f:
            theirs = json.load(f)
    except (OSError, ValueError):
        return 0
    if not isinstance(theirs, dict):
        return 0
    n = merge_into(store._d, theirs)
    if n:
        store.event(None, "state_external_write",
                    f"kept {n} row(s) another process wrote")
    return n

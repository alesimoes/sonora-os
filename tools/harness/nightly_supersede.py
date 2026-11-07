"""A green nightly on main supersedes the red ones before it.

A `main-red-<sha8>` task is only worth an engineer while main is red. When a
later nightly on main succeeds, every such task whose sha is an ancestor of
the green head describes a defect main no longer has (2026-11-07: a queued
task launched an engineer after the owner resumed the pools, though the
nightly had gone green meanwhile). Those tasks end "closed" and their active
run is stopped.
"""
from . import launcher

PREFIX = "main-red-"
LIVE = ("queued", "building")


def _stale(forge, task, head):
    """True when the task's sha is an ancestor of `head`; a forge error
    (sha not fetched, git missing) leaves the task alone."""
    sha8 = task["id"][len(PREFIX):]
    try:
        return bool(forge.is_ancestor(sha8, head))
    except Exception:
        return False


def _stop_runs(orch, task_id):
    for r in orch.store.runs_of(task_id):
        if r["ended"] is not None:
            continue
        launcher.kill_tree(r["pid"])
        proc = orch.procs.get(r["id"])
        orch._end_run(r, "superseded", proc.returncode if proc else None)


def close_superseded(orch, green):
    """Close the live main-red tasks the green run `green` supersedes;
    returns their ids."""
    store, head = orch.store, str(green.get("head_sha") or "")
    if not head:
        return []
    closed = []
    for t in store.tasks():
        if not t["id"].startswith(PREFIX) or t["state"] not in LIVE:
            continue
        if not _stale(orch.forge, t, head):
            continue
        _stop_runs(orch, t["id"])
        orch._release(t)
        store.advance(t["id"], "closed",
                      note=f"superseded by green nightly {green.get('id')}")
        store.event(t["id"], "superseded_by_green", str(green.get("id")))
        closed.append(t["id"])
    return closed

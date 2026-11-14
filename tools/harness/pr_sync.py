"""Keep a feature's tasks in step with its open PR.

The scheduler launches fixers only for tasks in ci_fix, and the CI watch
looks only at tasks in pr_open. A task requeued after an outage went
qa -> integrating while its feature sat in ci_fix with PR #528 red, and
nobody fixed it for 5 h (2026-11-08). Each tick the tasks are pulled back
to the state the feature is in, so the normal paths see them."""

STRAY = ("queued", "building", "bugfix", "qa", "integrating")


def _idle(store, t):
    return not any(r["task_id"] == t["id"] for r in store.active_runs())


def sync_tasks(store, f):
    """Move idle stray tasks of a feature with an open PR to ci_fix (feature
    in ci_fix) or pr_open (so the watch can find the PR red). Returns the
    ids moved."""
    if not f["pr"] or f["state"] in ("merged", "closed", "blocked"):
        return []
    target = "ci_fix" if f["state"] == "ci_fix" else "pr_open"
    moved = []
    for t in store.feature_tasks(f["id"]):
        if t["state"] in STRAY and _idle(store, t):
            old = t["state"]
            store.set_task(t["id"], state=target, attempts=0, cur_name=None,
                           not_before=0)
            store.event(t["id"], "state_synced",
                        f"{old}->{target} (PR #{f['pr']} open)")
            moved.append(t["id"])
    return moved

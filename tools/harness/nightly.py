"""Main's nightly full build (ADR-0061): the GitHub workflow
.github/workflows/nightly.yml, watched here; its red opens a fix task.

A PR pays its micro build only, so linking the application, the sanitizers,
the interop suites, the Windows build, the slow tests and the ISO run once a
day on main, on GitHub's runners. A red there is not a PR's fault and has no
PR to send a fixer to: it becomes a work task of its own, `main-red-<sha8>`,
queued for the track's engineer with the failure excerpt as the brief; the
scheduler launches it like any item and its fix arrives as a PR. One task
per failed run, however many ticks see it.
"""
import json

META_POLLED = "nightly-polled-at"


def tick(orch, now):
    """Every cfg.nightly_poll_s: the newest completed nightly run on main;
    a failed one not seen before opens its fix task."""
    store, cfg = orch.store, orch.cfg
    every = float(getattr(cfg, "nightly_poll_s", 900.0))
    try:
        last = float(store.get_meta(META_POLLED) or 0)
    except (TypeError, ValueError):
        last = 0.0
    if now - last < every:
        return None
    store.set_meta(META_POLLED, str(now))
    workflow = getattr(cfg, "nightly_workflow", "nightly.yml")
    try:
        runs = orch.forge.workflow_runs(workflow)
    except Exception as e:   # the board stands without the nightly card
        store.event("nightly", "poll_failed", str(e)[:200])
        return None
    done = [r for r in runs if r.get("status") == "completed"]
    if not done:
        return None
    r = done[0]
    key = f"nightly:{r.get('id')}"
    if store.get_meta(key):
        return None
    store.set_meta(key, r.get("conclusion") or "?")
    if r.get("conclusion") == "success":
        store.event("nightly", "nightly_green", str(r.get("head_sha", ""))[:12])
        return None
    excerpt = ""
    try:
        excerpt = orch.forge.failure_excerpt(r.get("head_sha", ""))
    except Exception:
        pass
    return open_fix_task(store, r.get("head_sha") or "", r, excerpt)


def open_fix_task(store, sha, run, excerpt):
    """The fix task for a red nightly run; None when the head has one."""
    tid = f"main-red-{sha[:8]}"
    if store.get_task(tid):
        return None
    brief = (
        f"main is red: the nightly full build ({run.get('url') or 'nightly.yml'}) "
        f"failed on {sha[:12]} with conclusion {run.get('conclusion')}.\n\n"
        "Find the cause (a defect on main, a test that assumes timing, or "
        "the environment), fix it at the root with a regression test, and "
        "open one PR against main. Never weaken an assertion or disable a "
        "check to make it pass. From the logs:\n"
        + (excerpt or "")[-3000:])
    with store.tx():
        store.add_task(tid, "daw", "rt-engineer", state="queued",
                       brief=brief, branch=f"fix/main-red-{sha[:8]}",
                       priority=-1)
    store.event(tid, "main_red", json.dumps({"sha": sha, "run": run.get("id")}))
    return tid

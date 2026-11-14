"""A stall alert gets an agent, not just a notification (owner, 2026-11-08).

When a feature has tasks stalled past `stall_alert_s` and nothing is already
working on its blocker, the daemon queues one principal-engineer run whose
brief is the stalled tasks, the blocked siblings' notes, the claims and the
dependencies. One at a time (`cfg.max_stall_resolvers`), and never twice for
the same feature within `stall_alert_s`. The run is a wake task (served by the
planner pool) with event `stall_resolver`; the console shows who is on it.
"""
import json

AGENT = "principal-engineer"
EVENT = "stall_resolver"
# Owner 2026-11-08/09: unblocking climbs a ladder, sonnet first, and only the
# product's core climbs past it: opus and fable go where a mistake breaks all
# of Sonora, the build or the OS (a feature with a high-risk slice). A plugin,
# a screen or one feature stays on sonnet. Past the ladder the feature is
# re-planned (architect; fable for the core) and the next stall re-plans again.
CORE_LADDER = ("sonnet", "opus", "fable")
PART_LADDER = ("sonnet", "sonnet")
REPLAN = "replan"
REPLAN_AGENT = "architect"
# A stall that comes back after this long starts at the bottom again.
LADDER_RESET_S = 6 * 3600
FIXING = ("fixer", "integrator", "principal")
DONE = ("blocked", "merged", "closed")


def _meta_key(fid):
    return f"stall_resolver:{fid}"


def is_core(store, fid):
    """A feature with a high-risk slice (grade "<size>/high", or only a
    "senior" tier when no grade was given)."""
    for t in store.feature_tasks(fid):
        try:
            d = json.loads(store.get_meta(f"tier:{t['id']}", "{}") or "{}")
        except ValueError:
            continue
        grade = str(d.get("grade") or "")
        if grade.endswith("/high") or (not grade and d.get("tier") == "senior"):
            return True
    return False


def _rung(store, fid, now, core):
    """The model of this try of fid (or REPLAN), and records the try."""
    key = f"stall_resolver_tries:{fid}"
    try:
        d = json.loads(store.get_meta(key, "{}") or "{}")
    except ValueError:
        d = {}
    n = d.get("n", 0) if now - d.get("ts", 0) < LADDER_RESET_S else 0
    store.set_meta(key, json.dumps({"n": n + 1, "ts": now}))
    ladder = CORE_LADDER if core else PART_LADDER
    return ladder[n] if n < len(ladder) else REPLAN


def _resolver_wakes(sv):
    return [w for w in sv.o.store.tasks(kind="wake")
            if w.get("event") == EVENT]


def _active_task_ids(sv):
    return {r["task_id"]: r for r in sv.o.store.active_runs()}


def _resolver_for(sv, fid):
    """(wake task, run or None) of a resolver queued or running for fid."""
    runs = _active_task_ids(sv)
    for w in _resolver_wakes(sv):
        if w["feature_id"] == fid and (w["state"] == "queued"
                                       or w["id"] in runs):
            return w, runs.get(w["id"])
    return None, None


def caring(sv, fid):
    """'harness cuidando: <agent>' text for the alert, or ''."""
    w, run = _resolver_for(sv, fid)
    if w is None:
        return ""
    return f"harness cuidando: {run['name'] if run else w['agent'] + ' (na fila)'}"


def _busy(sv):
    """Resolvers queued or running, against the cap."""
    runs = _active_task_ids(sv)
    return sum(1 for w in _resolver_wakes(sv)
               if w["state"] == "queued" or w["id"] in runs)


def _fixing_blocker(sv, fid):
    """A fixer, integrator or principal already runs on the feature."""
    ids = {t["id"] for t in sv.o.store.feature_tasks(fid)}
    return any(r["task_id"] in ids and r["role"] in FIXING
               for r in sv.o.store.active_runs())


def _recently_tried(sv, fid, now):
    last = float(sv.o.store.get_meta(_meta_key(fid), 0) or 0)
    return now - last < sv.th.stall_alert_s


def _why(sv, tid):
    for e in sv.o.store.events(tid, limit=60):
        if e.get("kind") in ("claim_denied", "pre_pr_failed", "launch_failed",
                             "run_failed", "reserve_failed", "retry_later"):
            return f"{e['kind']}: {(e.get('detail') or '')[:300]}"
    return "no recorded cause"


def brief(sv, fid, stalled):
    """stalled: [(task, age_seconds)]."""
    store = sv.o.store
    lines = [f"STALL RESOLVER: feature {fid} has tasks with no agent for "
             f"more than {sv.th.stall_alert_s / 3600:g} h and nothing is "
             "fixing the blocker. Find the root cause and clear it: fix the "
             "code or config in your own fresh worktree from origin/main "
             "(never edit the daemon's checkout, its state.json or "
             ".harness/ci), or say in result.json what the owner must "
             "decide. Never disable a check to get green.",
             f"If you open a PR for it, start its title with `{fid}: ` and "
             "put its branch in result.json `branch` (its number in `pr`): "
             "the daemon ties that PR to the feature and closes the feature "
             "when it merges. If only the owner can unblock it (a decision "
             "with no recommended default, or the host), report status "
             "blocked with `needs_owner` saying what the owner must decide.",
             "",
             "Stalled tasks:"]
    claims = set()
    for t, age in stalled:
        claims.update(t["files"] or ())
        lines.append(f"- {t['id']} in {t['state']} for {int(age // 60)} min; "
                     f"depends_on={t.get('depends_on') or []}; "
                     f"{_why(sv, t['id'])}")
    for t in store.feature_tasks(fid):
        if t["state"] == "blocked":
            lines.append(f"Blocked sibling {t['id']}: "
                         f"{(t.get('note') or 'no note')[:600]}")
    lines.append("Claims wanted: " + (", ".join(sorted(claims)) or "none"))
    try:
        owners = sv.o.broker.claim_owners()
    except Exception:  # the brief is useful without the holders
        owners = []
    if owners:
        lines.append("Claim holders now: " + ", ".join(sorted(owners)))
    return "\n".join(lines)


REPLAN_BRIEF = (
    "Re-plan this feature: the unblock tries did not move it (or it stalled "
    "again after a re-plan). Read the stall below, then rewrite the plan so "
    "every slice can finish: split or merge slices, fix files/claims "
    "overlaps and dependencies, drop what is already on main, grade each "
    "slice by risk. Write the machine plan to {plan} (schema "
    "planning/orchestrator/plan.schema.json; the daemon reads this file, "
    "not the markdown) and keep planning/plans/{fid}.md in step with it. "
    "When you finish, the daemon applies it: the old unfinished slices "
    "close, the plan's tasks start (start_from may name an old sub-branch "
    "to keep its commits) and the lead distributes them. Nothing is left "
    "to build (already on main): write it with \"tasks\": []. An "
    "unchanged plan file means you found nothing to change, and the "
    "feature then waits on the owner. A blocked slice under finished ones "
    "holds the whole feature (its integrator waits for every slice): "
    "re-scope it (widen its claim, split it, fix its dependency) so it can "
    "finish; leave it out of this plan only when what it lacks is hardware "
    "or an owner decision, and say so in the plan, so the finished slices "
    "merge now." + chr(10) * 2)


def replan_pending(store, fid):
    """An architect re-plan of fid queued and not yet served."""
    return any(w["state"] == "queued" and w["feature_id"] == fid
               and w.get("event") == EVENT and w["agent"] == REPLAN_AGENT
               for w in store.tasks(kind="wake"))


def queue_replan(o, fid, detail, model):
    """One architect re-plan run for fid (replan.py applies its plan).
    Returns the wake id."""
    store = o.store
    wid = f"wake-{REPLAN_AGENT}-{EVENT}-{fid}-{store.next_counter('wake')}"
    store.add_task(wid, None, REPLAN_AGENT, kind="wake", feature_id=fid,
                   brief=REPLAN_BRIEF.format(fid=fid,
                                             plan=o.flow.plan_file(fid))
                   + detail)
    store.set_task(wid, event=EVENT, model_pin=model)
    return wid


def _queue(sv, fid, stalled, now):
    store = sv.o.store
    core = is_core(store, fid)
    model = _rung(store, fid, now, core)
    n = None if model == REPLAN else store.next_counter("wake")
    if model == REPLAN:
        wid = queue_replan(sv.o, fid, brief(sv, fid, stalled),
                           "fable" if core else "sonnet")
    else:
        wid = f"wake-principal-{EVENT}-{fid}-{n}"
        store.add_task(wid, None, AGENT, kind="wake", feature_id=fid,
                       brief=brief(sv, fid, stalled))
        store.set_task(wid, event=EVENT, model_pin=model)
    store.set_meta(_meta_key(fid), now)
    store.event(wid, "stall_resolver", json.dumps(
        {"feature": fid, "tasks": [t["id"] for t, _ in stalled]}))


def _waits_on_moving_dep(t, by_id, running):
    """True when a dependency of t, direct or through other dependencies,
    has a live run of any role: t is waiting, not stalled. Why: #448
    (2026-11-09) was queued for ui.workflow.windows while the contract its
    tasks waited on sat in QA with a tester running, because only
    fixer/integrator/principal runs counted as progress."""
    seen, todo = set(), list(t.get("depends_on") or ())
    while todo:
        dep = todo.pop()
        if dep in seen:
            continue
        seen.add(dep)
        if dep in running:
            return True
        d = by_id.get(dep)
        if d is not None:
            todo.extend(d.get("depends_on") or ())
    return False


def resolve(sv, now, stalled):
    """stalled: [(task, age)] for every work task past the bound with no
    run. Queues at most one resolver per call, within the cap."""
    cap = int(getattr(sv.o.cfg, "max_stall_resolvers", 1))
    running = _active_task_ids(sv)
    by_id = {t["id"]: t for t in sv.o.store.tasks()}
    stalled = [(t, age) for t, age in stalled
               if not _waits_on_moving_dep(t, by_id, running)]
    by_feature = {}
    for t, age in stalled:
        by_feature.setdefault(t["feature_id"] or t["id"], []).append((t, age))
    for fid, rows in sorted(by_feature.items()):
        if _busy(sv) >= cap:
            return
        if _resolver_for(sv, fid)[0] or _recently_tried(sv, fid, now) \
                or _fixing_blocker(sv, fid):
            continue
        sv.act("stall", f"resolver for {fid}",
               f"{len(rows)} stalled task(s) past {sv.th.stall_alert_s:g} s",
               lambda fid=fid, rows=rows: _queue(sv, fid, rows, now))

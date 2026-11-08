"""Nothing stalls silently (2026-11-08 audit).

The supervisor already watched red PRs with no fixer, the lock and the disk.
Everything else could sit for hours with no alert: contract tasks refused a
claim every tick (an architect kept 23 files after its run), features in
`planning` waiting for the one architect seat, a feature whose `waits_on`
names a blocked feature, a PR open and not merging. These checks raise a
dashboard alert for any task, feature or PR with no run and no progress
past a bound, with the reason the daemon knows, and re-issue a lost wake.
"""
from . import stall_resolver, wakes
from .scheduler import DONE_STATES, HOLD
from .supervisor_checks import (_active, _feature_has_run, _hms,
                                _waiting_since, _why_not_launched)

TASK_STATES = ("queued", "building", "bugfix", "qa", "integrating")
PLANNING = {"planning": ("architect", "plan_request"),
            "distributing": ("lead", "plan_ready")}


def check_stalls(sv, now):
    stalled_tasks(sv, now)
    stalled_wakes(sv, now)
    stalled_features(sv, now)
    stalled_prs(sv, now)


def _last_event(sv, tid, kinds):
    for e in reversed(sv.o.store.events(tid, limit=60)):
        if e.get("kind") in kinds:
            return f"{e['kind']}: {(e.get('detail') or '')[:160]}"
    return ""


def _has_run(sv, t):
    if t["state"] == "integrating":   # the integrator runs on the first task
        return _feature_has_run(sv, t["feature_id"], t["id"])
    return any(r["task_id"] == t["id"] for r in _active(sv))


def _waiting_siblings(sv, t):
    """The feature's tasks that keep its integrator from launching
    (scheduler: one integrator per feature, once every slice is done).
    2026-11-08: five 'integrating with no agent' alerts were one blocked
    docs sibling each, and the text named neither the sibling nor its note."""
    if t["state"] != "integrating" or not t.get("feature_id"):
        return []
    return [s for s in sv.o.store.feature_tasks(t["feature_id"])
            if s["state"] not in DONE_STATES]


def _why_stalled(sv, t, now):
    sibs = _waiting_siblings(sv, t)
    if not sibs:
        return _why_not_launched(sv, t, now)
    return "integrator waits for " + ", ".join(
        f"{s['id']} ({s['state']}" + (f": {s['note'][:120]}" if s.get("note")
                                      else "") + ")" for s in sibs)


def stalled_tasks(sv, now):
    stalled = []
    for t in sv.o.store.tasks(kind="work"):
        if t["state"] not in TASK_STATES or t["not_before"] >= HOLD:
            continue
        if _has_run(sv, t):
            sv.forget("prs", (t["id"], t["state"]))
            continue
        age = _waiting_since(sv, t, now)
        if age <= sv.th.stall_alert_s:
            continue
        stalled.append((t, age))
    stall_resolver.resolve(sv, now, stalled)
    for t, age in stalled:
        why = _last_event(sv, t["id"], ("claim_denied", "pre_pr_failed",
                                        "pr_open_failed", "pr_deferred",
                                        "reserve_failed", "launch_failed",
                                        "run_failed", "retry_later"))
        care = stall_resolver.caring(sv, t["feature_id"] or t["id"])
        sv.alert(f"stall:{t['id']}",
                 f"{t['id']} sits in {t['state']} with no agent for "
                 f"{_hms(age)}: {_why_stalled(sv, t, now)}"
                 + (f"; last: {why}" if why else "")
                 + (f"; {care}" if care else ""), check="stall")


def stalled_wakes(sv, now):
    for w in sv.o.store.tasks(kind="wake"):
        if w["state"] != "queued" or _has_run(sv, w):
            continue
        age = _waiting_since(sv, w, now)
        if age > sv.th.stall_alert_s:
            sv.alert(f"stall:{w['id']}",
                     f"{w['agent']} wake {w['id']} not served for "
                     f"{_hms(age)}: {_why_not_launched(sv, w, now)}",
                     check="stall")


def _wake_pending(sv, fid, role, kind):
    prefix = f"wake-{role}-{kind}-{fid}"
    ws = sv.o.store.tasks(kind="wake")
    if wakes.is_pending(ws, prefix):
        return True
    ids = {w["id"] for w in ws if w["id"].startswith(prefix)}
    return any(r["task_id"] in ids for r in _active(sv))


def _rewake(sv, f, role, kind):
    flow, fid = sv.o.flow, f["id"]
    info = flow.info(fid)
    if kind == "plan_request":
        body = {"feature": fid, "track": f["track"],
                "backlog_ids": info.get("backlog_ids", []),
                "reserved": flow.reserved_for(fid)}
        brief = f"Plan feature {fid} ({f['track']})."
    else:
        body = {"feature": fid, "plan": flow.plan_file(fid)}
        brief = f"Distribute the plan of feature {fid}."
    sv.o._ensure_wake(role, kind, fid, brief=brief, feature=fid, body=body)


def stalled_features(sv, now):
    store = sv.o.store
    for f in store.features():
        fid = f["id"]
        if f["state"] in PLANNING:
            if store.feature_tasks(fid):
                continue   # an implicit one-task feature: its task is watched
            role, kind = PLANNING[f["state"]]
            if _wake_pending(sv, fid, role, kind):
                sv.forget("feat", fid)
                continue
            age = now - min(sv.remember("feat", fid, now),
                            f.get("updated") or now)
            if age <= sv.th.stall_alert_s:
                continue
            sv.act("stall", f"rewake {role} for {fid}",
                   f"feature in {f['state']} for {_hms(age)} with no "
                   f"{kind} wake pending or running",
                   lambda f=f, role=role, kind=kind: _rewake(sv, f, role,
                                                             kind))
            sv.alert(f"stall:{fid}", f"feature {fid} sat in {f['state']} "
                     f"for {_hms(age)} with no {role} wake; re-woken",
                     level="warn", check="stall")
        elif f["state"] == "queued":
            dead = _waits_on_dead(sv, f)
            if dead:
                sv.alert(f"waits:{fid}", f"feature {fid} waits on "
                         f"{', '.join(dead)}, which is blocked and will "
                         "never merge: unblock or close it", check="stall")


def _waits_on_dead(sv, f):
    """Blocked features named by this one's waits_on, when every feature it
    waits on is blocked (a live one would still make it wait)."""
    flow = sv.o.flow
    text = flow.info(f["id"]).get("waits_on") or ""
    if not text:
        return []
    hit, dead = [], []
    for o in sv.o.store.features():
        if o["id"] == f["id"] or o["state"] in ("merged", "closed"):
            continue
        ids = {o["id"], *flow.info(o["id"]).get("backlog_ids", [])}
        if any(i and i in text for i in ids):
            hit.append(o)
            if o["state"] == "blocked":
                dead.append(o["id"])
    return dead if hit and len(dead) == len(hit) else []


def stalled_prs(sv, now):
    rows = sv.o.train._all()
    for f in sv.o.store.features():
        if not f["pr"] or f["state"] != "pr_open":
            continue
        ts = sv.o.store.feature_tasks(f["id"])
        if not ts or _feature_has_run(sv, f["id"], ts[0]["id"]):
            continue
        age = _waiting_since(sv, ts[0], now)
        if age <= sv.th.pr_stall_s:
            continue
        why = (rows.get(f["id"]) or {}).get("next") or _last_event(
            sv, ts[0]["id"], ("ci_poll_failed", "feature_merge_failed",
                              "rate_limited", "train_error")) \
            or "checks not all green, or GitHub still computing mergeable"
        sv.alert(f"prstall:{f['id']}",
                 f"PR #{f['pr']} ({f['id']}) open and not merged for "
                 f"{_hms(age)}: {why}", check="stall")

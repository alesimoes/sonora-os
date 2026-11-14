"""The supervisor's checks, one function each: check(sv, now). They read the
store, the lock directory, the disk and the forge through `sv`, heal through
sv.act() (logged with the reason) and raise sv.alert() for what they cannot.

Which failure of the week each check would have caught:
  check_runs    an agent stuck with no output is nudged, then killed by the
                heartbeat; a run waiting in the lock queue is "building"
                (scheduler.holder_in_list), not stalled (an agent never
                finished because a process it spawned sat in the lock queue)
  check_lock    dead or orphan holders and waiters are reaped: a gate step
                left behind after its batch, a build an ended agent left in
                the queue, a ticket of a dead shell (orphan gate processes
                and stray waiters held or queued on the lock)
  check_queues  every head in the builder's queues belongs to an open PR
                (the builder spent ~5 h gating the head of a merged PR);
                every open PR is adopted (the daemon missed auto-adopt)
  check_prs     a red or conflicting PR whose task has no run for longer
                than unowned_max_s is woken or alerted (red and conflicting
                PRs waited with no one assigned)
  check_disk    low disk wakes the janitor; critical disk pauses the build
                pools (the daemon died when the disk filled; a fixer
                misattributed a red to disk space: the alert names it)
"""

from . import supervisor_sys as S
from .status import write_atomic
from .supervisor_dash import check_dashboard
from .supervisor_mail import check_undeliverable

PAUSED_META = "supervisor:paused"
BUILD_POOLS = ("build", "qa")
CLAIM_GRACE_S = 120.0


def _active(sv):
    return sv.o.store.active_runs()


def _hms(s):
    s = int(max(s, 0))
    return f"{s // 3600}h{(s % 3600) // 60:02d}" if s >= 3600 else \
        f"{s // 60} min"


# -- runs -----------------------------------------------------------------
def check_runs(sv, now):
    """A suspect run (no beat for cfg.suspect_s, not building) is told once
    to report; the heartbeat kills and relaunches it at stalled_s."""
    # A run whose worktree holds or waits for the heavy lock is building,
    # whatever its stream says; read the lock directory once, not bash.
    lock_cwds = [e["cwd"] for e in sv.sys.lock_entries() if e.get("cwd")]
    for r in _active(sv):
        age = now - (r.get("last_beat") or r.get("started") or now)
        wt = r.get("worktree") or ""
        building = bool(wt) and any(S.under(c, wt) for c in lock_cwds)
        if r.get("suspect") and not building and sv.th.nudge and \
                not sv.remember("runs", ("nudged", r["id"]), False):
            sv.mem["runs"][("nudged", r["id"])] = True
            sv.act("runs", f"nudge {r['name']}",
                   f"run {r['id']} silent for {_hms(age)}",
                   lambda r=r: sv.o.broker.tell(
                       r["name"], "wake",
                       {"text": "The supervisor sees no output from you for "
                        f"{_hms(age)}. If you are waiting on a build or the "
                        "heavy lock, say so with `send lead`; otherwise "
                        "finish and write result.json."},
                       task=r["task_id"]))
        if r.get("suspect") and not building:
            sv.alert(f"run:{r['id']}",
                     f"{r['name']} ({r['task_id']}) silent for {_hms(age)}; "
                     f"killed at {_hms(sv.o.cfg.stalled_s)} without a beat",
                     level="warn", check="runs")
    for key in [k for k in sv.mem.get("runs", {})
                if k[1] not in {r["id"] for r in _active(sv)}]:
        sv.forget("runs", key)


# -- the heavy lock ---------------------------------------------------------
def lock_owner_ended(sv, cwd):
    """Id of the ended run that owns `cwd`, else None. Owner = the DEEPEST
    known run worktree containing cwd; agent worktrees nest inside the
    daemon checkout, where wake runs live, so the root never owns a
    nested cwd. A worktree an active run uses is not ended."""
    live = {S.posix(r.get("worktree") or "") for r in _active(sv)}
    root = S.posix(getattr(sv.o, "root", "") or "")
    best, best_run = "", None
    for r in sv.o.store._d["runs"].values():
        wt = S.posix(r.get("worktree") or "")
        if not wt or wt == root or not S.under(cwd, wt):
            continue
        if len(wt) > len(best):
            best, best_run = wt, r
    for wt in live:
        if wt and wt != root and S.under(cwd, wt) and len(wt) >= len(best):
            return None
    return best_run["id"] if best_run and best_run.get("ended") else None


def check_lock(sv, now):
    entries = sv.sys.lock_entries()
    if not entries:
        return
    for e in entries:
        where = f"{e['role']} pid={e['pid']} cwd={e['cwd']} cmd={e['cmd']}"
        age = now - (e["start"] or now)
        if e.get("claiming"):
            if age > CLAIM_GRACE_S and sv.th.lock_reap:
                sv.act("lock", "reap slot", f"claim without owner for "
                       f"{_hms(age)}: {e['path']}",
                       lambda e=e: sv.sys.lock_remove(e))
            continue
        alive = sv.sys.entry_alive(e)
        if alive is False and sv.th.lock_reap:
            sv.act("lock", f"reap {e['role']}", f"pid dead: {where}",
                   lambda e=e: sv.sys.lock_remove(e))
            continue
        owner = lock_owner_ended(sv, e["cwd"])
        if owner and sv.th.lock_reap:
            sv.act("lock", f"kill {e['role']}",
                   f"left behind by ended run {owner}: {where}",
                   lambda e=e: sv.sys.kill_entry(e) and sv.sys.lock_remove(e))
            continue
        limit = sv.th.lock_hold_alert_s if e["role"] == "holds" \
            else sv.th.lock_wait_alert_s
        if age > limit:
            sv.alert(f"lock:{e['role']}:{e['pid']}",
                     f"{e['role']} the heavy lock for {_hms(age)}: {where}",
                     check="lock")


# -- PRs the daemon's sweep missed --------------------------------------------
def _train_rows(sv):
    try:
        return sv.o.train._all()
    except Exception:
        return {}


def check_queues(sv, now):
    _adopt_missing(sv, now)


def _adopt_missing(sv, now):
    try:
        prs = sv.o.train._open_list(now)
    except Exception as e:
        sv.alert("queues:list", f"gh pr list failed: {e}", level="warn",
                 check="queues")
        return
    owned = {f["pr"] for f in sv.o.store.features() if f["pr"]}
    mine = {x["branch"] for x in (sv.o.store.features()
                                  + sv.o.store.tasks())
            if x["branch"] and not x["pr"]}
    missing = [p["number"] for p in prs
               if p["number"] not in owned and not p.get("draft")
               and p["branch"] not in mine]
    for n in [k for k in sv.mem.get("adopt", {}) if k not in missing]:
        sv.forget("adopt", n)
    # The daemon's own sweep (autoadopt.tick) is the door; this is the check
    # that it worked. A PR still unadopted after the grace is taken here.
    late = [n for n in missing
            if now - sv.remember("adopt", n, now) >= sv.th.adopt_grace_s]
    if not late:
        return
    got = []
    sv.act("queues", "adopt", "open PRs the auto-adopt sweep missed: "
           + " ".join(f"#{n}" for n in late),
           lambda: got.extend(sv.o.train.adopt(set(late), auto=True)))
    missing = late
    left = sorted(set(missing) - set(got))
    if left:
        sv.alert("queues:unadopted", "open PRs not on the train: "
                 + " ".join(f"#{n}" for n in left), check="queues")


# -- PRs with no owner --------------------------------------------------------
def _feature_has_run(sv, fid, tid):
    ids = {t["id"] for t in sv.o.store.feature_tasks(fid)} if fid else {tid}
    return any(r["task_id"] in ids for r in _active(sv))


def _why_not_launched(sv, t, now):
    paused = sv.o.paused()
    if paused & {"fix", "merge", t.get("track")}:
        return "pool paused: " + ", ".join(sorted(paused))
    if len(_active(sv)) >= sv.o.cfg.cap:
        return f"cap {sv.o.cfg.cap} reached"
    if t.get("not_before", 0) > now:
        return f"backoff until +{_hms(t['not_before'] - now)}"
    gap = sv.o.last_gaps.get(t.get("track")) if hasattr(sv.o, "last_gaps") \
        else None
    return f"scheduler gap: {gap}" if gap else \
        f"not launched (attempts {t.get('attempts')}, " \
        f"escalated {t.get('escalated') or 'no'})"


def _waiting_since(sv, t, now):
    """How long the task has sat in this state with no run. The store's
    `updated` moves on every write (a backoff, a note), so the supervisor
    also remembers when it first saw the wait; the older of the two wins,
    which survives a daemon restart as well."""
    key = (t["id"], t["state"])
    for k in [k for k in sv.mem.get("prs", {}) if k[0] == t["id"]
              and k != key]:
        sv.forget("prs", k)
    first = sv.remember("prs", key, now)
    return now - min(first, t.get("updated") or now)


def check_prs(sv, now):
    rows = _train_rows(sv)
    for t in sv.o.store.tasks(kind="work"):
        f = sv.o.store.get_feature(t["feature_id"]) or {}
        pr = f.get("pr") or t.get("pr")
        fid = t["feature_id"]
        if t["state"] == "ci_fix":
            if _feature_has_run(sv, fid, t["id"]):
                sv.forget("prs", (t["id"], t["state"]))
                continue
            age = _waiting_since(sv, t, now)
            if age <= sv.th.unowned_max_s:
                continue
            if t.get("not_before", 0) > now:
                sv.act("prs", f"wake {t['id']}",
                       f"PR #{pr} {t.get('fix_kind') or 'red'} with no "
                       f"fixer for {_hms(age)}; backoff cleared",
                       lambda t=t: sv.o.store.set_task(t["id"],
                                                       not_before=now))
                continue
            sv.alert(f"unowned:{t['id']}",
                     f"PR #{pr} {t.get('fix_kind') or 'red'} ({t['id']}) "
                     f"has no {('integrator' if t.get('fix_kind') in ('conflict', 'bundle') else 'fixer')} "
                     f"for {_hms(age)}: {_why_not_launched(sv, t, now)}",
                     check="prs")
        elif t["state"] == "pr_open" and pr:
            i = rows.get(fid) or {}
            if i.get("phase") in ("conflito", "fixer") and \
                    _waiting_since(sv, t, now) > sv.th.unowned_max_s and \
                    not _feature_has_run(sv, fid, t["id"]):
                sv.alert(f"unowned:{t['id']}",
                         f"PR #{pr} is {i['phase']} on the train but its "
                         f"task sits in pr_open for {_hms(age)}", check="prs")
        elif t["state"] == "blocked" and pr:
            sv.alert(f"blocked:{t['id']}",
                     f"PR #{pr} blocked: {(t.get('note') or '')[:160]}",
                     level="warn", check="prs")


# -- disk --------------------------------------------------------------------
def _set_paused(sv, pools):
    write_atomic(sv.o._p("paused.json"), sorted(pools))


def check_disk(sv, now):
    free = sv.sys.disk_free_gb(sv.o.root)
    if free is None:
        return
    store = sv.o.store
    paused = sv.o.paused()
    ours = set((store.get_meta(PAUSED_META) or "").split(",")) - {""}
    if free < sv.th.disk_critical_gb:
        add = set(BUILD_POOLS) - paused
        if add:
            sv.act("disk", "pause pools " + ",".join(sorted(add)),
                   f"{free:.0f} GB free < critical {sv.th.disk_critical_gb:g}",
                   lambda: (_set_paused(sv, paused | add),
                            store.set_meta(PAUSED_META,
                                           ",".join(sorted(ours | add)))))
        sv.alert("disk:critical",
                 f"disk critical: {free:.0f} GB free; build pools paused, "
                 "janitor woken. A red build now is the disk, not the code.",
                 check="disk")
    if free < sv.th.disk_low_gb:
        last = sv.remember("disk", "janitor", 0.0)
        if now - last >= sv.th.disk_janitor_every_s:
            sv.mem["disk"]["janitor"] = now
            sv.act("disk", "wake janitor", f"{free:.0f} GB free < "
                   f"{sv.th.disk_low_gb:g}",
                   lambda: sv.o._ensure_wake(
                       "janitor", "janitor", f"disk-{int(now)}",
                       f"Disk low: {free:.0f} GB free. Reclaim merged agent "
                       "worktrees, stale build dirs, docker images and "
                       "volumes of ours only. Never another project's."))
        if free >= sv.th.disk_critical_gb:
            sv.alert("disk:low", f"disk low: {free:.0f} GB free "
                     f"(janitor runs every {_hms(sv.th.disk_janitor_every_s)})",
                     level="warn", check="disk")
    elif ours:
        sv.act("disk", "resume pools " + ",".join(sorted(ours)),
               f"{free:.0f} GB free again",
               lambda: (_set_paused(sv, paused - ours),
                        store.set_meta(PAUSED_META, "")))


DAEMON_CHECKS = (check_runs, check_lock, check_queues, check_prs, check_disk,
                 check_dashboard, check_undeliverable)

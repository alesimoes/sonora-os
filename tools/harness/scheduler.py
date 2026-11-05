"""Staffing policy (ADR-0047 section 5): pure decisions, no I/O.

plan() takes a snapshot and returns which tasks get an agent this tick. The
daemon executes the answer; nothing here launches, kills or writes.
"""
import os
from dataclasses import dataclass, field

try:
    import tomllib
except ImportError:  # Python < 3.11
    tomllib = None

HARD_CAP = 20
# Priority classes, lowest number first: fix > tester > integrator > engineer
# (below its track floor) > lead/architect > burst engineer.
P_FIX, P_TESTER, P_INTEGRATOR, P_ENGINEER, P_PLANNER, P_BURST = range(6)
POOL_OF_KIND = {"fixer": "fix", "tester": "qa", "integrator": "merge",
                "engineer": "build", "planner": "plan"}
KIND_OF_STATE = {"ci_fix": "fixer", "qa": "tester", "integrating": "integrator",
                 "queued": "engineer", "building": "engineer",
                 "bugfix": "engineer"}
# Claims are held while a task is anywhere between first launch and the PR.
CLAIMING_STATES = ("building", "bugfix", "qa", "integrating")
DONE_STATES = ("integrating", "pr_open", "ci_fix", "merged")


def default_local_builds(cpus=None):
    cpus = cpus or os.cpu_count() or 2
    return max(1, min(4, cpus // 4))


def max_agents_cap(env=None):
    env = os.environ if env is None else env
    cap = HARD_CAP
    raw = env.get("CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS")
    if raw:
        try:
            cap = min(cap, int(raw))
        except ValueError:
            pass
    return max(cap, 0)


@dataclass
class Config:
    cap: int = field(default_factory=max_agents_cap)
    floors: dict = field(default_factory=lambda: {"daw": 8, "os": 4,
                                                  "plugin": 2})
    max_testers: int = 4
    max_fixers: int = 4
    max_integrators: int = 1
    max_open_prs: int = 10
    max_ready_unpr: int = 6
    local_builds: int = field(default_factory=default_local_builds)
    max_runs_per_hour: int = 60
    max_turns: int = 80
    tick_s: float = 30.0
    working_s: float = 600.0
    suspect_s: float = 900.0
    stalled_s: float = 1800.0
    build_s: float = 3600.0
    tester_s: float = 2700.0
    ceiling_s: float = 10800.0
    max_attempts: int = 3
    backoff_s: tuple = (60.0, 300.0, 900.0)
    pause_default_s: float = 1800.0
    ci_poll_s: float = 120.0    # between `gh pr checks` calls per PR
    janitor_every: int = 3      # merges between janitor runs
    done_grace_s: float = 30.0  # wait for result.json after a `done` message
    permission_mode: str = "acceptEdits"
    seeds: dict = field(default_factory=dict)  # kind -> last used number
    live_pools: object = None  # None: every pool is live when not dry-run

    def __post_init__(self):
        self.cap = min(int(self.cap), HARD_CAP)


def load_config(path=None, env=None):
    cfg = Config(cap=max_agents_cap(env))
    if path and os.path.exists(path) and tomllib is not None:
        with open(path, "rb") as f:
            data = tomllib.load(f)
        for k, v in data.items():
            if k == "floors":
                cfg.floors = dict(v)
            elif k == "seeds":
                cfg.seeds = dict(v)
            elif k == "backoff_s":
                cfg.backoff_s = tuple(float(x) for x in v)
            elif hasattr(cfg, k):
                setattr(cfg, k, v)
        cfg.cap = min(int(cfg.cap), max_agents_cap(env))
    return cfg


def globs_overlap(a, b):
    """Conservative: two globs overlap when their literal prefixes nest."""
    def prefix(g):
        i = min([n for n in (g.find(c) for c in "*?[") if n >= 0]
                or [len(g)])
        return g[:i]
    pa, pb = prefix(a), prefix(b)
    return pa.startswith(pb) or pb.startswith(pa)


def files_overlap(xs, ys):
    return any(globs_overlap(a, b) for a in xs for b in ys)


@dataclass
class Launch:
    task_id: str
    kind: str
    pool: str
    priority: int
    reason: str = ""


@dataclass
class Plan:
    launches: list = field(default_factory=list)
    gaps: dict = field(default_factory=dict)
    wakes: list = field(default_factory=list)  # (role, kind, detail)
    seats_free: int = 0


def plan(tasks, runs, cfg, now, stop=False, usage_pause_until=0.0,
         paused=(), hour_budget=None):
    """tasks: store task dicts. runs: active runs as dicts with keys task_id,
    role (kind), working (bool), building (bool), name."""
    out = Plan()
    live = len(runs)
    seats = max(cfg.cap - live, 0)
    if hour_budget is not None:
        seats = min(seats, max(hour_budget, 0))
    out.seats_free = seats
    by_id = {t["id"]: t for t in tasks}
    running = {r["task_id"] for r in runs}
    running_names = {r.get("name") for r in runs}
    count = {k: sum(1 for r in runs if r["role"] == k)
             for k in POOL_OF_KIND}
    building = sum(1 for r in runs if r.get("building"))
    working_eng = {}
    for r in runs:
        if r["role"] == "engineer" and r.get("working"):
            tr = by_id.get(r["task_id"], {}).get("track")
            working_eng[tr] = working_eng.get(tr, 0) + 1

    work = [t for t in tasks if t["kind"] == "work"]
    by_feature = {}
    for t in work:
        by_feature.setdefault(t["feature_id"] or t["id"], []).append(t)
    # One PR per feature, however many tasks it has.
    open_prs = len({t["feature_id"] or t["id"] for t in work
                    if t["state"] in ("pr_open", "ci_fix")})
    ready_unpr = sum(1 for t in tasks if t["state"] == "integrating")
    claims = [t["files"] for t in tasks if t["state"] in CLAIMING_STATES
              and t["kind"] == "work" and t["files"]]

    block = None
    if stop:
        block = "kill switch"
    elif usage_pause_until > now:
        block = "usage pause"
    elif seats <= 0:
        block = "cap" if live >= cfg.cap else "hourly run limit"

    def paused_for(pool, track):
        return pool in paused or (track is not None and track in paused)

    def ready(t):
        return t["id"] not in running and t["not_before"] <= now

    cands = []  # (class, task priority, created, task, kind)
    eligible_by_track = {}
    for t in tasks:
        kind = "planner" if t["kind"] == "wake" and t["state"] == "queued" \
            else KIND_OF_STATE.get(t["state"]) if t["kind"] == "work" else None
        if kind is None or not ready(t):
            continue
        if kind in ("fixer", "integrator"):
            sibs = by_feature[t["feature_id"] or t["id"]]
            if t["id"] != sibs[0]["id"]:
                continue  # the feature's first task stands for it
            if kind == "integrator" and (t.get("integrated") or any(
                    s["state"] not in DONE_STATES for s in sibs)):
                continue  # waiting for sibling tasks, or PR being opened
            if kind == "fixer" and t.get("fix_kind") == "conflict":
                kind = "integrator"
        if kind == "engineer":
            if not store_deps_ok(t, by_id):
                continue
            mine = t["files"]
            if t["state"] == "queued" and mine and any(
                    files_overlap(mine, c) for c in claims):
                continue
            eligible_by_track[t["track"]] = \
                eligible_by_track.get(t["track"], 0) + 1
        cls = {"fixer": P_FIX, "tester": P_TESTER, "integrator": P_INTEGRATOR,
               "engineer": P_ENGINEER, "planner": P_PLANNER}[kind]
        if t["state"] == "ci_fix":
            cls = P_FIX  # a conflict run is as urgent as a CI fix
        cands.append((cls, t["priority"], t["created"], t["id"], kind))
    cands.sort()

    launched = 0
    new_claims = []

    def can(kind, t):
        pool = POOL_OF_KIND[kind]
        if paused_for(pool, t["track"] if kind != "planner" else None):
            return False
        if kind == "tester" and (count["tester"] >= cfg.max_testers
                                 or building >= cfg.local_builds):
            return False
        if kind == "fixer" and count["fixer"] >= cfg.max_fixers:
            return False
        if kind == "integrator" and (
                count["integrator"] >= cfg.max_integrators
                or (t["state"] != "ci_fix" and open_prs >= cfg.max_open_prs)):
            return False
        if kind == "engineer":
            if ready_unpr >= cfg.max_ready_unpr:
                return False
            if t["state"] == "queued" and t["files"] and any(
                    files_overlap(t["files"], c) for c in new_claims):
                return False
        if kind == "planner" and t.get("agent") in running_names:
            return False
        return True

    def take(cls, kind, tid, reason=""):
        nonlocal launched, seats
        t = by_id[tid]
        out.launches.append(Launch(tid, kind, POOL_OF_KIND[kind], cls, reason))
        seats -= 1
        launched += 1
        count[kind] += 1
        if kind == "engineer":
            working_eng[t["track"]] = working_eng.get(t["track"], 0) + 1
            if t["files"]:
                new_claims.append(t["files"])
        if kind == "planner":
            running_names.add(t.get("agent"))

    taken = set()
    if block is None:
        # Pass 1: fix, tester, integrator; then engineers below their floor,
        # round-robin across tracks so one track cannot starve another.
        for cls, _, _, tid, kind in cands:
            if seats <= 0:
                break
            if kind not in ("planner", "engineer") and can(kind, by_id[tid]):
                take(cls, kind, tid)
                taken.add(tid)
        queues = {}
        for cls, _, _, tid, kind in cands:
            if kind == "engineer":
                queues.setdefault(by_id[tid]["track"], []).append(tid)
        progress = True
        while seats > 0 and progress:
            progress = False
            for track in sorted(queues):
                if seats <= 0:
                    break
                if working_eng.get(track, 0) >= cfg.floors.get(track, 0):
                    continue
                while queues[track]:
                    tid = queues[track].pop(0)
                    if can("engineer", by_id[tid]):
                        take(P_ENGINEER, "engineer", tid)
                        taken.add(tid)
                        progress = True
                        break
        # Pass 2: lead / architect wake runs borrow a free seat.
        for cls, _, _, tid, kind in cands:
            if seats <= 0:
                break
            if kind == "planner" and can(kind, by_id[tid]):
                take(cls, kind, tid)
                taken.add(tid)
        # Pass 3: burst engineers while seats remain.
        for cls, _, _, tid, kind in cands:
            if seats <= 0:
                break
            if kind == "engineer" and tid not in taken \
                    and can(kind, by_id[tid]):
                take(P_BURST, kind, tid, "burst")
                taken.add(tid)

    for track, floor in sorted(cfg.floors.items()):
        have = working_eng.get(track, 0)
        if have >= floor:
            continue
        if block:
            out.gaps[track] = block
        elif track in paused or "build" in paused:
            out.gaps[track] = "paused"
        elif ready_unpr >= cfg.max_ready_unpr or (
                open_prs >= cfg.max_open_prs and ready_unpr):
            out.gaps[track] = "CI budget"
        elif seats <= 0:
            out.gaps[track] = "cap"
        elif eligible_by_track.get(track, 0) == 0:
            out.gaps[track] = "no eligible task"
            out.wakes.append(("lead", "pool_under_floor", track))
        else:
            out.gaps[track] = "files claimed or dependencies pending"
    out.seats_free = seats
    return out


def store_deps_ok(t, by_id):
    for dep in t["depends_on"]:
        d = by_id.get(dep)
        if d is None or d["state"] not in ("integrating", "pr_open",
                                           "ci_fix", "merged"):
            return False
    return True


def holds_build_slot(root, worktree):
    """True when tools/build-slot.sh (or its fallback) holds a lock for this
    worktree: a lock directory named after it under .harness/build-slots."""
    if not worktree:
        return False
    return os.path.exists(os.path.join(root, ".harness", "build-slots",
                                       os.path.basename(worktree)))

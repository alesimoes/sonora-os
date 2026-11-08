"""State store of the orchestrator (ADR-0047 sections 2 and 4a).

One JSON document, `state.json`, held in memory by the daemon (the only
writer) and replaced atomically on disk: write a temp file in the same
directory, flush and fsync it, os.replace it over the old one, fsync the
directory where the platform allows. A crash therefore leaves the previous
complete file or the new complete file, never half of one.

Saving policy: every mutating call that is not inside tx() saves before it
returns; tx() saves once when its outermost block exits cleanly and restores
the in-memory state when the block raises. The one exception is a run's
heartbeat (`last_beat`, `suspect`): it only marks the document dirty, because
adopt_runs() resets both after a restart and an fsync per run per tick buys
nothing. The tick ends with flush(), which saves whatever is dirty. A failed
save raises, leaves the old file untouched and keeps the document dirty, so
the next save retries.

Events go to `events.jsonl` (append-only, one JSON object per line, flushed,
rotated to `events.<n>.jsonl` at about 5 MB). Rows that fail validation on
load go to `state.quarantine.json` instead of stopping the daemon.

Task state changes go through advance() only; no role writes state.
"""
import json
import os
import pickle
import sqlite3
import tempfile
import threading
import time

from . import store_guard

# state -> states it may move to. "blocked" and "closed" (PR closed without
# merge) are reachable from everywhere; merged and closed are terminal.
TRANSITIONS = {
    "queued": {"building", "designing"},
    "designing": {"building"},
    "building": {"qa", "building"},
    "qa": {"integrating", "bugfix", "qa"},
    "bugfix": {"qa", "bugfix"},
    "integrating": {"pr_open", "integrating"},
    "pr_open": {"merged", "ci_fix"},
    "ci_fix": {"pr_open", "ci_fix"},
    "merged": set(),
    "closed": set(),
    "blocked": {"queued", "building", "qa", "bugfix", "integrating",
                "pr_open", "ci_fix"},
}
STATES = frozenset(TRANSITIONS)
# States in which a task is waiting for an agent (no run needed otherwise).
AGENT_STATES = ("queued", "building", "bugfix", "qa", "integrating", "ci_fix")
OPEN_PR_STATES = ("pr_open", "ci_fix")
# Runs that do the work and count against max_runs_per_hour. Planner runs
# (lead, architect, janitor) mostly last 1-2 minutes; counting them let 28
# of them use up an hour's budget while 3 engineers worked (2026-11-08).
WORK_ROLES = ("engineer", "tester", "integrator", "fixer", "principal")
PLANNER_ROLES = ("planner",)
NUMBER_KINDS = ("adr", "protocol", "format", "window")

SCHEMA_VERSION = 1
EVENT_ROTATE_BYTES = 5 * 1024 * 1024
# Heartbeat fields: a change to only these does not force a save.
VOLATILE_RUN_FIELDS = frozenset({"last_beat", "suspect"})

FEATURE_COLS = {
    "id": None, "plan_path": None, "branch": None, "track": None,
    "state": "planning", "pr": None, "adr_no": None, "protocol_no": None,
    "partial": 0, "created": None, "updated": None, "fixes": 0,
    "job_reds": "{}", "red_since": None, "principal_runs": 0,
}
TASK_COLS = {
    "id": None, "kind": "work", "backlog_id": None, "feature_id": None,
    "track": None, "agent": None, "state": None, "branch": None,
    "worktree": None, "pr": None, "adr_no": None, "protocol_no": None,
    "attempts": 0, "bounces": 0, "depends_on": [], "files": [], "brief": "",
    "eng_name": None, "cur_name": None, "wants": "", "not_before": 0,
    "priority": 0, "created": None, "updated": None, "note": "",
    "integrated": 0, "fix_kind": None, "escalated": None, "event": None, "start_from": None,
    "owner": None, "cause": None,
}
RUN_COLS = {
    "id": None, "task_id": None, "role": None, "name": None, "state": None,
    "pid": None, "started": None, "last_beat": None, "ended": None,
    "exit": None, "result_path": None, "stream_path": None, "worktree": None,
    "session_id": None, "suspect": 0, "outcome": None, "model": None,
}


class StateError(Exception):
    pass


def _row(cols, src):
    """A row with every known column present; unknown keys are kept."""
    out = {}
    for k, default in cols.items():
        v = src.get(k, default)
        out[k] = list(v) if isinstance(v, list) else v
    for k, v in src.items():
        out.setdefault(k, v)
    return out


def _empty():
    return {"schema": SCHEMA_VERSION, "features": {}, "tasks": {}, "runs": {},
            "reservations": [], "meta": {}}


def _fsync_dir(d):
    try:
        fd = os.open(d, os.O_RDONLY)
    except OSError:      # Windows cannot open a directory
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def write_json_atomic(path, doc):
    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".state-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
            f.flush()
            os.fsync(f.fileno())
        for i in range(5):
            try:
                os.replace(tmp, path)
                break
            except PermissionError:   # Windows: a reader holds the target
                if i == 4:
                    raise
                time.sleep(0.05 * (i + 1))
        _fsync_dir(d)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


class Store:
    def __init__(self, path, clock=time.time):
        self.path = str(path)
        self.clock = clock
        self._dir = os.path.dirname(os.path.abspath(self.path))
        self._lock = threading.RLock()
        self._depth = 0
        self._dirty = False
        self._snap = None
        self.events_path = os.path.join(self._dir, "events.jsonl")
        self.quarantine_path = os.path.join(
            self._dir, os.path.splitext(os.path.basename(self.path))[0]
            + ".quarantine.json")
        self._d = _empty()
        self._sig = store_guard.LOADING   # the file is the source: no merge
        self._load()
        self._sig = store_guard.signature(self.path)

    def close(self):
        self.flush()

    def flush(self):
        """Save if anything changed since the last save."""
        with self._lock:
            if self._dirty:
                self._save()

    # -- persistence --------------------------------------------------
    def _save(self):
        # another writer's additions are kept, not erased (store_guard.py)
        store_guard.merge_external(self)
        write_json_atomic(self.path, self._d)
        self._sig = store_guard.signature(self.path)
        self._dirty = False

    def _mutated(self, volatile=False):
        self._dirty = True
        if not volatile and self._depth == 0:
            self._save()

    def _load(self):
        legacy = os.path.splitext(self.path)[0] + ".db"
        if not os.path.exists(self.path):
            if os.path.exists(legacy):
                self._migrate(legacy)
            return
        try:
            with open(self.path, encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            raise StateError(f"{self.path} is unreadable: {e}")
        if not isinstance(doc, dict) or "schema" not in doc:
            raise StateError(f"{self.path} has no schema field")
        if doc["schema"] != SCHEMA_VERSION:
            raise StateError(f"{self.path}: schema {doc['schema']!r}, this "
                             f"daemon reads {SCHEMA_VERSION}")
        base = _empty()
        for k in ("features", "tasks", "runs", "meta"):
            v = doc.get(k, base[k])
            base[k] = v if isinstance(v, dict) else {}
        r = doc.get("reservations", [])
        base["reservations"] = r if isinstance(r, list) else []
        self._d = base
        if self._validate():
            self._save()

    def _validate(self):
        """Move rows that break the invariants into the quarantine file.
        Returns True when anything moved."""
        d, bad = self._d, []

        def drop(table, key, row, why):
            bad.append({"ts": self.clock(), "table": table, "key": key,
                        "reason": why, "row": row})

        feats = {}
        for k, r in d["features"].items():
            if not isinstance(r, dict) or r.get("id") != k:
                drop("features", k, r, "malformed row")
                continue
            r = _row(FEATURE_COLS, r)
            if r["pr"] is not None and any(
                    o["pr"] == r["pr"] for o in feats.values()):
                drop("features", k, r, f"duplicate pr {r['pr']}")
                continue
            feats[k] = r
        tasks = {}
        for k, r in d["tasks"].items():
            if not isinstance(r, dict) or r.get("id") != k:
                drop("tasks", k, r, "malformed row")
                continue
            r = _row(TASK_COLS, r)
            if r["state"] not in STATES:
                drop("tasks", k, r, f"unknown state {r['state']!r}")
            elif r["kind"] == "work" and r["feature_id"] not in feats:
                drop("tasks", k, r, f"feature {r['feature_id']!r} missing")
            else:
                tasks[k] = r
        runs = {}
        for k, r in d["runs"].items():
            if not isinstance(r, dict) or r.get("id") != k:
                drop("runs", k, r, "malformed row")
                continue
            r = _row(RUN_COLS, r)
            if r["task_id"] not in tasks:
                drop("runs", k, r, f"task {r['task_id']!r} missing")
            elif r["state"] is not None and r["state"] not in STATES:
                drop("runs", k, r, f"unknown state {r['state']!r}")
            else:
                runs[k] = r
        resv, seen = [], set()
        for r in d["reservations"]:
            ok = (isinstance(r, dict) and r.get("kind") in NUMBER_KINDS
                  and isinstance(r.get("number"), int)
                  and (r["kind"], r["number"]) not in seen)
            if not ok:
                drop("reservations", None, r, "malformed or duplicate")
                continue
            seen.add((r["kind"], r["number"]))
            resv.append({"kind": r["kind"], "number": r["number"],
                         "task_id": r.get("task_id")})
        d["features"], d["tasks"], d["runs"] = feats, tasks, runs
        d["reservations"] = resv
        d["meta"] = {str(k): str(v) for k, v in d["meta"].items()}
        if not bad:
            return False
        old = []
        try:
            with open(self.quarantine_path, encoding="utf-8") as f:
                old = json.load(f)
        except (OSError, ValueError):
            pass
        write_json_atomic(self.quarantine_path,
                          (old if isinstance(old, list) else []) + bad)
        for b in bad:
            tid = b["key"] if b["table"] in ("tasks", "runs") else None
            self.event(tid, "quarantined",
                       f"{b['table']}/{b['key']}: {b['reason']}")
        self._dirty = True
        return True

    def _migrate(self, legacy):
        """One-time import of the old SQLite file; the file is renamed to
        state.db.migrated afterwards and never opened again."""
        db = sqlite3.connect("file:" + legacy.replace("\\", "/")
                             .replace("?", "%3f") + "?mode=ro", uri=True)
        db.row_factory = sqlite3.Row
        d = _empty()
        try:
            def rows(table):
                try:
                    return [dict(r) for r in db.execute(
                        f"SELECT * FROM {table} ORDER BY rowid")]
                except sqlite3.Error:
                    return []
            for r in rows("features"):
                d["features"][r["id"]] = _row(FEATURE_COLS, r)
            for r in rows("tasks"):
                for c in ("depends_on", "files"):
                    r[c] = json.loads(r.get(c) or "[]")
                d["tasks"][r["id"]] = _row(TASK_COLS, r)
            for r in rows("runs"):
                d["runs"][r["id"]] = _row(RUN_COLS, r)
            d["reservations"] = [
                {"kind": r["kind"], "number": r["number"],
                 "task_id": r["task_id"]} for r in rows("reservations")]
            d["meta"] = {r["key"]: str(r["value"]) for r in rows("meta")}
            events = rows("events")
        finally:
            db.close()
        if events and not os.path.exists(self.events_path):
            with open(self.events_path, "w", encoding="utf-8") as f:
                for e in events:
                    f.write(json.dumps({k: e.get(k) for k in (
                        "ts", "task_id", "kind", "detail")}) + "\n")
        self._d = d
        self._validate()
        self._save()
        for suffix in ("", "-wal", "-shm"):
            if os.path.exists(legacy + suffix):
                os.replace(legacy + suffix, legacy + suffix + ".migrated")
        self.event(None, "migrated", "state.db -> state.json")

    # -- transactions -------------------------------------------------
    def tx(self):
        return _Tx(self)

    # -- events -------------------------------------------------------
    def event(self, task_id, kind, detail=""):
        if not isinstance(detail, str):
            detail = json.dumps(detail, sort_keys=True)
        line = json.dumps({"ts": self.clock(), "task_id": task_id,
                           "kind": kind, "detail": detail}) + "\n"
        with self._lock:
            try:
                if os.path.getsize(self.events_path) >= EVENT_ROTATE_BYTES:
                    os.replace(self.events_path, os.path.join(
                        self._dir, f"events.{self._next_rotation()}.jsonl"))
            except OSError:
                pass
            with open(self.events_path, "a", encoding="utf-8") as f:
                f.write(line)
                f.flush()

    def _rotated(self):
        out = []
        for n in os.listdir(self._dir):
            p = n.split(".")
            if len(p) == 3 and p[0] == "events" and p[2] == "jsonl" \
                    and p[1].isdigit():
                out.append((int(p[1]), os.path.join(self._dir, n)))
        return sorted(out)

    def _next_rotation(self):
        r = self._rotated()
        return r[-1][0] + 1 if r else 1

    def events(self, task_id=None, kind=None, limit=200):
        """Newest first."""
        files = [p for _, p in reversed(self._rotated())]
        files.insert(0, self.events_path)
        out = []
        with self._lock:
            for p in files:
                try:
                    with open(p, encoding="utf-8") as f:
                        lines = f.read().splitlines()
                except OSError:
                    continue
                for ln in reversed(lines):
                    try:
                        e = json.loads(ln)
                    except ValueError:    # a torn last line after a crash
                        continue
                    if task_id is not None and e.get("task_id") != task_id:
                        continue
                    if kind is not None and e.get("kind") != kind:
                        continue
                    out.append(e)
                    if len(out) >= limit:
                        return out
        return out

    # -- meta ---------------------------------------------------------
    def next_counter(self, key):
        with self._lock:
            n = int(self._d["meta"].get(key, 0)) + 1
            self._d["meta"][key] = str(n)
            self._mutated()
        return n

    def get_meta(self, key, default=None):
        return self._d["meta"].get(key, default)

    def set_meta(self, key, value):
        with self._lock:
            self._d["meta"][key] = str(value)
            self._mutated()

    # -- features -----------------------------------------------------
    def add_feature(self, id, track, branch=None, plan_path=None,
                    partial=False):
        now = self.clock()
        with self._lock:
            if id in self._d["features"]:
                raise StateError(f"feature {id} exists")
            self._d["features"][id] = _row(FEATURE_COLS, {
                "id": id, "plan_path": plan_path, "branch": branch,
                "track": track, "partial": int(partial), "created": now,
                "updated": now})
            self._mutated()

    def ensure_feature(self, id, track, branch=None):
        now = self.clock()
        with self._lock:
            if id in self._d["features"]:
                return
            self._d["features"][id] = _row(FEATURE_COLS, {
                "id": id, "branch": branch, "track": track, "created": now,
                "updated": now})
            self._mutated()

    def get_feature(self, id):
        r = self._d["features"].get(id)
        return dict(r) if r else None

    def features(self):
        with self._lock:
            return sorted((dict(r) for r in self._d["features"].values()),
                          key=lambda r: r["created"] or 0)

    def set_feature(self, id, **fields):
        pr = fields.get("pr")
        with self._lock:
            if pr is not None and any(
                    f["pr"] == pr and f["id"] != id
                    for f in self._d["features"].values()):
                raise StateError(f"PR {pr} already belongs to another "
                                 f"feature")
            self._update("features", id, fields)

    # -- tasks --------------------------------------------------------
    def add_task(self, id, track, agent, state="queued", kind="work",
                 depends_on=(), files=(), brief="", feature_id=None,
                 backlog_id=None, branch=None, priority=0, wants=()):
        now = self.clock()
        if state not in STATES:
            raise StateError(f"unknown state {state}")
        with self._lock:
            if id in self._d["tasks"]:
                raise StateError(f"task {id} exists")
            fid = feature_id
            if kind == "work":
                # Every item is a feature; a lone task is a one-task feature.
                fid = feature_id or id
                if fid not in self._d["features"]:
                    self._d["features"][fid] = _row(FEATURE_COLS, {
                        "id": fid, "track": track, "created": now,
                        "updated": now})
            self._d["tasks"][id] = _row(TASK_COLS, {
                "id": id, "kind": kind, "backlog_id": backlog_id,
                "feature_id": fid, "track": track, "agent": agent,
                "state": state, "branch": branch or f"feat/{id}",
                "depends_on": list(depends_on), "files": list(files),
                "brief": brief, "priority": priority,
                "wants": ",".join(wants), "created": now, "updated": now})
            self._dirty = True
            self.event(id, "task_added", state)
            self._mutated()

    def get_task(self, id):
        r = self._d["tasks"].get(id)
        return _copy(r) if r else None

    def tasks(self, states=None, kind=None):
        with self._lock:
            out = [r for r in self._d["tasks"].values()
                   if (not states or r["state"] in states)
                   and (not kind or r["kind"] == kind)]
            out.sort(key=lambda r: (r["priority"] or 0, r["created"] or 0))
            return [_copy(r) for r in out]

    def set_task(self, id, **fields):
        self._update("tasks", id, fields)

    def _update(self, table, id, fields):
        if not fields:
            return
        with self._lock:
            row = self._d[table].get(id)
            if row is None:
                return
            row.update(fields, updated=self.clock())
            self._mutated()

    def advance(self, id, new_state, note=None):
        """The one state-machine entry point. Resets attempts and the
        per-state agent name when the state really changes."""
        with self.tx():
            t = self.get_task(id)
            if t is None:
                raise StateError(f"no task {id}")
            old = t["state"]
            if new_state not in ("blocked", "closed") \
                    and new_state not in TRANSITIONS[old]:
                raise StateError(f"{id}: {old} -> {new_state} not allowed")
            if new_state == old and new_state in ("blocked", "closed"):
                return
            if old in ("merged", "closed"):
                raise StateError(f"{id}: {old} is terminal")
            fields = {"state": new_state}
            if new_state != old:
                fields.update(attempts=0, cur_name=None, not_before=0,
                              owner=None, cause=None)
            if note is not None:
                fields["note"] = note
            self.set_task(id, **fields)
            self.event(id, "state", f"{old}->{new_state}"
                       + (f" {note}" if note else ""))
        return old

    # -- features as a unit -------------------------------------------
    def feature_tasks(self, fid):
        """Work tasks of a feature in merge order (priority, creation)."""
        return [t for t in self.tasks() if t["feature_id"] == fid
                and t["kind"] == "work"]

    def advance_feature(self, fid, new_state, note=None, **fields):
        """Move every live task of a feature together (PR open, CI fix,
        merged). Blocked and merged tasks stay where they are."""
        with self.tx():
            for t in self.feature_tasks(fid):
                if t["state"] in ("blocked", "merged", "closed"):
                    continue
                self.advance(t["id"], new_state, note=note)
                if fields:
                    self.set_task(t["id"], **fields)
            self.set_feature(fid, state=new_state)

    def has_deps_done(self, task):
        for dep in task["depends_on"]:
            d = self.get_task(dep)
            if d is None or d["state"] not in (
                    "integrating", "pr_open", "ci_fix", "merged"):
                return False
        return True

    # -- runs ---------------------------------------------------------
    def add_run(self, **f):
        with self._lock:
            if f.get("id") in self._d["runs"]:
                raise StateError(f"run {f.get('id')} exists")
            self._d["runs"][f["id"]] = _row(RUN_COLS, f)
            self._mutated()

    def get_run(self, id):
        r = self._d["runs"].get(id)
        return dict(r) if r else None

    def set_run(self, id, **fields):
        with self._lock:
            row = self._d["runs"].get(id)
            if row is None or not fields:
                return
            row.update(fields)
            self._mutated(volatile=set(fields) <= VOLATILE_RUN_FIELDS)

    def _sorted_runs(self, pred):
        with self._lock:
            rs = [r for r in self._d["runs"].values() if pred(r)]
            rs.sort(key=lambda r: r["started"] or 0)
            return rs

    def active_runs(self):
        return [dict(r) for r in self._sorted_runs(
            lambda r: r["ended"] is None)]

    def runs_of(self, task_id):
        return [dict(r) for r in self._sorted_runs(
            lambda r: r["task_id"] == task_id)]

    def last_run_of_name(self, name):
        rs = self._sorted_runs(lambda r: r["name"] == name)
        return dict(rs[-1]) if rs else None

    def last_run_name(self, task_id, role):
        rs = self._sorted_runs(
            lambda r: r["task_id"] == task_id and r["role"] == role)
        return rs[-1]["name"] if rs else None

    def runs_started_since(self, ts, roles=None):
        return len(self._sorted_runs(
            lambda r: r["started"] is not None and r["started"] >= ts
            and (roles is None or r["role"] in roles)))

    # -- number reservation ------------------------------------------
    def reserve(self, kind, task_id, seed):
        """Next number of `kind`: max(seed, every reservation) + 1. A task
        gets the same number back when it asks twice."""
        if kind not in NUMBER_KINDS:
            raise StateError(f"unknown number kind {kind}")
        with self.tx():
            mine = [r for r in self._d["reservations"] if r["kind"] == kind]
            for r in mine:
                if r["task_id"] == task_id:
                    return r["number"]
            n = max([int(seed)] + [r["number"] for r in mine]) + 1
            self._d["reservations"].append(
                {"kind": kind, "number": n, "task_id": task_id})
            self._mutated()
            self.event(task_id, "reserved", f"{kind}={n}")
        return n

    def reservations(self):
        with self._lock:
            return sorted((dict(r) for r in self._d["reservations"]),
                          key=lambda r: (r["kind"], r["number"]))


def _copy(row):
    t = dict(row)
    t["depends_on"] = list(t["depends_on"])
    t["files"] = list(t["files"])
    return t


class _Tx:
    """Re-entrant; nested use joins the outer block. The outermost block
    saves once on a clean exit and restores the document on an exception."""

    def __init__(self, store):
        self.s = store

    def __enter__(self):
        s = self.s
        s._lock.acquire()
        if s._depth == 0:
            s._snap = (pickle.dumps(s._d), s._dirty)
        s._depth += 1

    def __exit__(self, et, ev, tb):
        s = self.s
        s._depth -= 1
        try:
            if s._depth == 0:
                snap, was_dirty = s._snap
                s._snap = None
                if et:
                    s._d = pickle.loads(snap)
                    s._dirty = was_dirty
                elif s._dirty:
                    s._save()
        finally:
            s._lock.release()
        return False

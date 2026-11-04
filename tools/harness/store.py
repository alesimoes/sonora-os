"""State store of the orchestrator (ADR-0047 sections 2 and 4a).

One SQLite file in WAL mode. Every mutation runs in BEGIN IMMEDIATE, so the
daemon and a manual CLI call cannot interleave. Task state changes go through
advance() only; no role writes state.
"""
import json
import sqlite3
import threading
import time

# state -> states it may move to. "blocked" is reachable from everywhere.
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
    "blocked": {"queued", "building", "qa", "bugfix", "integrating",
                "pr_open", "ci_fix"},
}
STATES = frozenset(TRANSITIONS)
# States in which a task is waiting for an agent (no run needed otherwise).
AGENT_STATES = ("queued", "building", "bugfix", "qa", "integrating", "ci_fix")
OPEN_PR_STATES = ("pr_open", "ci_fix")
NUMBER_KINDS = ("adr", "protocol", "format")

SCHEMA = """
CREATE TABLE IF NOT EXISTS features(
  id TEXT PRIMARY KEY, plan_path TEXT, branch TEXT, track TEXT,
  state TEXT DEFAULT 'planning', pr INTEGER, adr_no INTEGER,
  protocol_no INTEGER, partial INTEGER DEFAULT 0, created REAL, updated REAL);
CREATE UNIQUE INDEX IF NOT EXISTS features_pr ON features(pr)
  WHERE pr IS NOT NULL;
CREATE TABLE IF NOT EXISTS tasks(
  id TEXT PRIMARY KEY, kind TEXT DEFAULT 'work', backlog_id TEXT,
  feature_id TEXT, track TEXT, agent TEXT, state TEXT, branch TEXT,
  worktree TEXT, pr INTEGER, adr_no INTEGER, protocol_no INTEGER,
  attempts INTEGER DEFAULT 0, bounces INTEGER DEFAULT 0,
  depends_on TEXT DEFAULT '[]', files TEXT DEFAULT '[]', brief TEXT DEFAULT '',
  eng_name TEXT, cur_name TEXT, wants TEXT DEFAULT '', not_before REAL DEFAULT 0, priority INTEGER
  DEFAULT 0, created REAL, updated REAL, note TEXT DEFAULT '');
CREATE TABLE IF NOT EXISTS runs(
  id TEXT PRIMARY KEY, task_id TEXT, role TEXT, name TEXT, state TEXT,
  pid INTEGER, started REAL, last_beat REAL, ended REAL, exit INTEGER,
  result_path TEXT, stream_path TEXT, worktree TEXT, session_id TEXT,
  suspect INTEGER DEFAULT 0, outcome TEXT);
CREATE TABLE IF NOT EXISTS events(
  ts REAL, task_id TEXT, kind TEXT, detail TEXT);
CREATE TABLE IF NOT EXISTS reservations(
  kind TEXT, number INTEGER, task_id TEXT, PRIMARY KEY(kind, number));
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
"""


class StateError(Exception):
    pass


class Store:
    def __init__(self, path, clock=time.time):
        self.path = str(path)
        self.clock = clock
        self._lock = threading.RLock()
        self._db = sqlite3.connect(self.path, timeout=30,
                                   isolation_level=None,
                                   check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA busy_timeout=30000")
        self._db.executescript(SCHEMA)
        self._depth = 0

    def close(self):
        self._db.close()

    # -- transactions -------------------------------------------------
    def tx(self):
        return _Tx(self)

    def _q(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args).fetchall()

    def _x(self, sql, args=()):
        with self._lock:
            return self._db.execute(sql, args)

    # -- events -------------------------------------------------------
    def event(self, task_id, kind, detail=""):
        if not isinstance(detail, str):
            detail = json.dumps(detail, sort_keys=True)
        self._x("INSERT INTO events VALUES(?,?,?,?)",
                (self.clock(), task_id, kind, detail))

    def events(self, task_id=None, kind=None, limit=200):
        sql, args, cond = "SELECT * FROM events", [], []
        if task_id is not None:
            cond.append("task_id=?")
            args.append(task_id)
        if kind is not None:
            cond.append("kind=?")
            args.append(kind)
        if cond:
            sql += " WHERE " + " AND ".join(cond)
        sql += " ORDER BY rowid DESC LIMIT ?"
        return [dict(r) for r in self._q(sql, (*args, limit))]

    # -- meta ---------------------------------------------------------
    def next_counter(self, key):
        with self.tx():
            row = self._q("SELECT value FROM meta WHERE key=?", (key,))
            n = int(row[0]["value"]) + 1 if row else 1
            self._x("INSERT OR REPLACE INTO meta VALUES(?,?)", (key, str(n)))
        return n

    def get_meta(self, key, default=None):
        row = self._q("SELECT value FROM meta WHERE key=?", (key,))
        return row[0]["value"] if row else default

    def set_meta(self, key, value):
        self._x("INSERT OR REPLACE INTO meta VALUES(?,?)", (key, str(value)))

    # -- features -----------------------------------------------------
    def add_feature(self, id, track, branch=None, plan_path=None,
                    partial=False):
        now = self.clock()
        self._x("INSERT INTO features(id,plan_path,branch,track,partial,"
                "created,updated) VALUES(?,?,?,?,?,?,?)",
                (id, plan_path, branch, track, int(partial), now, now))

    def get_feature(self, id):
        r = self._q("SELECT * FROM features WHERE id=?", (id,))
        return dict(r[0]) if r else None

    def features(self):
        return [dict(r) for r in self._q("SELECT * FROM features "
                                         "ORDER BY created")]

    def set_feature(self, id, **fields):
        self._update("features", id, fields)

    # -- tasks --------------------------------------------------------
    def add_task(self, id, track, agent, state="queued", kind="work",
                 depends_on=(), files=(), brief="", feature_id=None,
                 backlog_id=None, branch=None, priority=0, wants=()):
        now = self.clock()
        if state not in STATES:
            raise StateError(f"unknown state {state}")
        self._x("INSERT INTO tasks(id,kind,backlog_id,feature_id,track,agent,"
                "state,branch,depends_on,files,brief,priority,wants,created,updated)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (id, kind, backlog_id, feature_id, track, agent, state,
                 branch or f"feat/{id}", json.dumps(list(depends_on)),
                 json.dumps(list(files)), brief, priority, ",".join(wants),
                 now, now))
        self.event(id, "task_added", state)

    def get_task(self, id):
        r = self._q("SELECT * FROM tasks WHERE id=?", (id,))
        return _task(r[0]) if r else None

    def tasks(self, states=None, kind=None):
        sql, args, cond = "SELECT * FROM tasks", [], []
        if states:
            cond.append("state IN (%s)" % ",".join("?" * len(states)))
            args += list(states)
        if kind:
            cond.append("kind=?")
            args.append(kind)
        if cond:
            sql += " WHERE " + " AND ".join(cond)
        return [_task(r) for r in self._q(sql + " ORDER BY priority, created",
                                          args)]

    def set_task(self, id, **fields):
        self._update("tasks", id, fields)

    def _update(self, table, id, fields):
        if not fields:
            return
        fields = dict(fields, updated=self.clock())
        cols = ",".join(f"{k}=?" for k in fields)
        self._x(f"UPDATE {table} SET {cols} WHERE id=?",
                (*fields.values(), id))

    def advance(self, id, new_state, note=None):
        """The one state-machine entry point. Resets attempts and the
        per-state agent name when the state really changes."""
        with self.tx():
            t = self.get_task(id)
            if t is None:
                raise StateError(f"no task {id}")
            old = t["state"]
            if new_state != "blocked" and new_state not in TRANSITIONS[old]:
                raise StateError(f"{id}: {old} -> {new_state} not allowed")
            if new_state == "blocked" and old == "blocked":
                return
            fields = {"state": new_state}
            if new_state != old:
                fields.update(attempts=0, cur_name=None, not_before=0)
            if note is not None:
                fields["note"] = note
            self.set_task(id, **fields)
            self.event(id, "state", f"{old}->{new_state}"
                       + (f" {note}" if note else ""))
        return old

    def has_deps_done(self, task):
        for dep in task["depends_on"]:
            d = self.get_task(dep)
            if d is None or d["state"] not in (
                    "integrating", "pr_open", "ci_fix", "merged"):
                return False
        return True

    # -- runs ---------------------------------------------------------
    def add_run(self, **f):
        cols = ",".join(f)
        self._x(f"INSERT INTO runs({cols}) VALUES({','.join('?' * len(f))})",
                tuple(f.values()))

    def get_run(self, id):
        r = self._q("SELECT * FROM runs WHERE id=?", (id,))
        return dict(r[0]) if r else None

    def set_run(self, id, **fields):
        cols = ",".join(f"{k}=?" for k in fields)
        self._x(f"UPDATE runs SET {cols} WHERE id=?", (*fields.values(), id))

    def active_runs(self):
        return [dict(r) for r in self._q(
            "SELECT * FROM runs WHERE ended IS NULL ORDER BY started")]

    def runs_of(self, task_id):
        return [dict(r) for r in self._q(
            "SELECT * FROM runs WHERE task_id=? ORDER BY started", (task_id,))]

    def last_run_of_name(self, name):
        r = self._q("SELECT * FROM runs WHERE name=? ORDER BY started DESC "
                    "LIMIT 1", (name,))
        return dict(r[0]) if r else None

    def last_run_name(self, task_id, role):
        r = self._q("SELECT name FROM runs WHERE task_id=? AND role=? "
                    "ORDER BY started DESC LIMIT 1", (task_id, role))
        return r[0]["name"] if r else None

    def runs_started_since(self, ts):
        return self._q("SELECT COUNT(*) c FROM runs WHERE started>=?",
                       (ts,))[0]["c"]

    # -- number reservation ------------------------------------------
    def reserve(self, kind, task_id, seed):
        """Next number of `kind`: max(seed, every reservation) + 1. A task
        gets the same number back when it asks twice."""
        if kind not in NUMBER_KINDS:
            raise StateError(f"unknown number kind {kind}")
        with self.tx():
            r = self._q("SELECT number FROM reservations WHERE kind=? AND "
                        "task_id=?", (kind, task_id))
            if r:
                return r[0]["number"]
            m = self._q("SELECT MAX(number) m FROM reservations WHERE kind=?",
                        (kind,))[0]["m"] or 0
            n = max(int(seed), m) + 1
            self._x("INSERT INTO reservations VALUES(?,?,?)",
                    (kind, n, task_id))
            self.event(task_id, "reserved", f"{kind}={n}")
        return n

    def reservations(self):
        return [dict(r) for r in self._q("SELECT * FROM reservations "
                                         "ORDER BY kind, number")]


def _task(row):
    t = dict(row)
    t["depends_on"] = json.loads(t["depends_on"] or "[]")
    t["files"] = json.loads(t["files"] or "[]")
    return t


class _Tx:
    """Re-entrant BEGIN IMMEDIATE; nested use joins the outer transaction."""

    def __init__(self, store):
        self.s = store

    def __enter__(self):
        self.s._lock.acquire()
        if self.s._depth == 0:
            self.s._db.execute("BEGIN IMMEDIATE")
        self.s._depth += 1

    def __exit__(self, et, ev, tb):
        self.s._depth -= 1
        try:
            if self.s._depth == 0:
                self.s._db.execute("ROLLBACK" if et else "COMMIT")
        finally:
            self.s._lock.release()
        return False

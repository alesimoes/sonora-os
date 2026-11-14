"""Rotating on-disk backups of state.json, and the start check that uses
them (2026-11-14: git deleted state.json during a forced self-update and the
daemon came back on an empty store; the coordinator rebuilt it by hand).

BackedStore is the daemon's Store: after a save, at most every EVERY_S, it
writes the document to state.json.bak.1 (atomically, through the Store's
own write_json_atomic) after shifting .1 -> .2 -> .3; the last KEEP stay.
An empty document (no features, no tasks) is never backed up, so a wiped
store cannot rotate the good copies out.

refuse_start() is asked before the daemon (or a CLI writer) opens the
store: when state.json is missing or empty and a backup younger than
RECENT_S holds features or tasks, it returns the reason and appends a
`store_empty_refused` alert to events.jsonl; the caller does not start.
"""
import json
import os
import time

from .store import Store, write_json_atomic

KEEP = 3
EVERY_S = 300
RECENT_S = 24 * 3600


def backup_path(path, n):
    return f"{path}.bak.{n}"


def has_content(doc):
    return isinstance(doc, dict) and bool(doc.get("features")
                                          or doc.get("tasks"))


def _mtime(path):
    try:
        return os.path.getmtime(path)
    except OSError:
        return None


class Backups:
    def __init__(self, path, clock=time.time, every_s=EVERY_S, keep=KEEP):
        self.path, self.clock = str(path), clock
        self.every_s, self.keep = every_s, keep
        self.last = _mtime(backup_path(self.path, 1))

    def due(self, now):
        return self.last is None or now - self.last >= self.every_s

    def _rotate(self):
        for n in range(self.keep, 1, -1):
            src = backup_path(self.path, n - 1)
            if os.path.exists(src):
                os.replace(src, backup_path(self.path, n))

    def maybe(self, doc):
        """True when a backup was written now."""
        now = self.clock()
        if not has_content(doc) or not self.due(now):
            return False
        self._rotate()
        write_json_atomic(backup_path(self.path, 1), doc)
        self.last = now
        return True


class BackedStore(Store):
    """Store whose saves also keep the rotating backups. A failed backup
    never fails the save that triggered it; the next save retries."""

    def __init__(self, path, clock=time.time, backups=None):
        self.backups = backups or Backups(path)
        super().__init__(path, clock)

    def _save(self):
        super()._save()
        try:
            self.backups.maybe(self._d)
        except OSError:
            pass

    def mark_dirty(self):
        """The next flush rewrites state.json from memory (self-update put
        back a file git had removed or changed)."""
        with self._lock:
            self._dirty = True


def _load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _counts(doc):
    return len(doc.get("features") or {}), len(doc.get("tasks") or {})


def best_backup(path, now, keep=KEEP, recent_s=RECENT_S):
    """(path, doc, age) of the newest recent backup with content, or None."""
    found = []
    for n in range(1, keep + 1):
        p = backup_path(path, n)
        m = _mtime(p)
        if m is None or now - m > recent_s:
            continue
        doc = _load(p)
        if has_content(doc):
            found.append((m, p, doc))
    if not found:
        return None
    m, p, doc = max(found, key=lambda x: x[0])
    return p, doc, now - m


def _alert(odir, now, detail):
    line = json.dumps({"ts": now, "task_id": None,
                       "kind": "store_empty_refused", "detail": detail})
    try:
        with open(os.path.join(odir, "events.jsonl"), "a",
                  encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def refuse_start(odir, now=None):
    """None when the store may be opened, else the reason it may not."""
    now = time.time() if now is None else now
    path = os.path.join(odir, "state.json")
    if os.path.exists(path) and has_content(_load(path)):
        return None
    best = best_backup(path, now)
    if best is None:
        return None
    bak, doc, age = best
    feats, tasks = _counts(doc)
    why = (f"ALERT state.json is missing or empty but {bak} "
           f"({feats} features, {tasks} tasks, {age / 60:.0f} min old) "
           f"exists: copy it over state.json to restore, or move the "
           f"state.json.bak.* files away to start on an empty store")
    _alert(odir, now, why)
    return why

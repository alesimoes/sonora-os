"""Rotating backups of state.json and the empty-store start check
(2026-11-14: the daemon restarted on a store git had deleted)."""
import io
import json
import os
import shutil
import tempfile
import types
import unittest
from contextlib import redirect_stderr

import support  # noqa: F401  (sets sys.path)
from harness import cli, store_backup
from harness.store_backup import BackedStore, Backups, backup_path


class Clock:
    def __init__(self, t=10_000.0):
        self.t = t

    def __call__(self):
        return self.t


class Rotation(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.path = os.path.join(self.tmp, "state.json")
        self.clock = Clock()
        self.b = Backups(self.path, clock=self.clock, every_s=300, keep=3)

    def doc(self, n):
        return {"features": {f"f{n}": {}}, "tasks": {}}

    def read(self, n):
        with open(backup_path(self.path, n)) as f:
            return json.load(f)

    def test_keeps_the_last_three_newest_first(self):
        for n in range(5):
            self.assertTrue(self.b.maybe(self.doc(n)))
            self.clock.t += 300
        self.assertEqual([list(self.read(i)["features"]) for i in (1, 2, 3)],
                         [["f4"], ["f3"], ["f2"]])
        self.assertFalse(os.path.exists(backup_path(self.path, 4)))
        self.assertEqual(sorted(os.listdir(self.tmp)),
                         ["state.json.bak.1", "state.json.bak.2",
                          "state.json.bak.3"])   # no temp file left

    def test_written_at_most_every_interval(self):
        self.assertTrue(self.b.maybe(self.doc(1)))
        self.clock.t += 299
        self.assertFalse(self.b.maybe(self.doc(2)))
        self.clock.t += 1
        self.assertTrue(self.b.maybe(self.doc(2)))

    def test_an_empty_store_never_rotates_the_good_copies_out(self):
        self.b.maybe(self.doc(1))
        for _ in range(4):
            self.clock.t += 300
            self.assertFalse(self.b.maybe({"features": {}, "tasks": {}}))
        self.assertEqual(list(self.read(1)["features"]), ["f1"])

    def test_the_daemon_store_backs_itself_up(self):
        s = BackedStore(self.path, backups=self.b)
        s._d["features"]["f9"] = {"id": "f9"}
        s.mark_dirty()
        s.flush()
        self.assertIn("f9", self.read(1)["features"])


class EmptyStart(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(self.odir)
        self.state = os.path.join(self.odir, "state.json")

    def backup(self, age_s=60, n=1):
        p = backup_path(self.state, n)
        with open(p, "w") as f:
            json.dump({"features": {"a": {}}, "tasks": {"t": {}}}, f)
        t = 1_000_000.0 - age_s
        os.utime(p, (t, t))

    def events(self):
        with open(os.path.join(self.odir, "events.jsonl")) as f:
            return [json.loads(ln)["kind"] for ln in f]

    def test_missing_store_with_a_recent_backup_is_refused(self):
        self.backup()
        why = store_backup.refuse_start(self.odir, now=1_000_000.0)
        self.assertIn("state.json.bak.1", why)
        self.assertIn("1 features, 1 tasks", why)
        self.assertEqual(self.events(), ["store_empty_refused"])

    def test_an_empty_store_counts_as_missing(self):
        with open(self.state, "w") as f:
            json.dump({"schema": 1, "features": {}, "tasks": {}}, f)
        self.backup()
        self.assertTrue(store_backup.refuse_start(self.odir,
                                                  now=1_000_000.0))

    def test_starts_without_backups_old_backups_or_with_content(self):
        self.assertIsNone(store_backup.refuse_start(self.odir, 1_000_000.0))
        self.backup(age_s=store_backup.RECENT_S + 1)
        self.assertIsNone(store_backup.refuse_start(self.odir, 1_000_000.0))
        self.backup()
        with open(self.state, "w") as f:
            json.dump({"schema": 1, "features": {"x": {}}, "tasks": {}}, f)
        self.assertIsNone(store_backup.refuse_start(self.odir, 1_000_000.0))

    def test_the_daemon_command_refuses_and_never_opens_the_store(self):
        self.backup(age_s=0, n=1)
        os.utime(backup_path(self.state, 1))   # now, on the real clock
        built = []
        args = types.SimpleNamespace(shadow=False, shadow_agents=None)
        err = io.StringIO()
        with redirect_stderr(err):
            rc = cli._run(None, args, self.tmp,
                          lambda *a, **k: built.append(1), None)
        self.assertEqual(rc, 2)
        self.assertEqual(built, [])
        self.assertIn("refusing to start", err.getvalue())
        self.assertFalse(os.path.exists(self.state))


if __name__ == "__main__":
    unittest.main()

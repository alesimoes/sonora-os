"""harness/triage_pr.py: 'No commits between main and <branch>' when the
daemon opens a PR."""
import unittest

import support
from harness import triage_pr
from harness.forge import ForgeError

NOCOMMIT = ForgeError("gh pr create: No commits between main and feat/x")


class Unit(unittest.TestCase):
    def test_error_and_kind_detection(self):
        self.assertTrue(triage_pr.is_no_commits(NOCOMMIT))
        self.assertFalse(triage_pr.is_no_commits(ForgeError("push failed")))
        self.assertTrue(triage_pr.is_already_on_main_kind(
            [{"id": "verify-x", "branch": "feat/x"}]))
        self.assertTrue(triage_pr.is_already_on_main_kind(
            [{"id": "x", "branch": "feat/y", "brief": "adr-pointer for ADR-0009"}]))
        self.assertFalse(triage_pr.is_already_on_main_kind(
            [{"id": "x", "branch": "feat/y", "brief": "build the meter"}]))


class Opening(support.DaemonCase):
    def feature(self, o, tid, brief=""):
        o.store.add_task(tid, "daw", "rt-engineer", files=[f"{tid}/**"],
                         feature_id=tid, state="integrating", brief=brief)
        o.store.set_task(tid, integrated=1)

    def open(self, o, tid):
        f = o.store.get_feature(tid) or {"id": tid, "pr": None}
        ts = o.store.feature_tasks(tid)
        self.assertTrue(triage_pr.handle(o, f, ts, NOCOMMIT, 100.0))

    def test_verify_item_is_closed_as_already_on_main(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        self.feature(o, "verify-meters")
        self.open(o, "verify-meters")
        self.assertEqual(o.store.get_task("verify-meters")["state"], "closed")

    def test_build_task_is_sent_back_once_then_escalated(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        self.feature(o, "meters", brief="build the meter")
        self.open(o, "meters")
        t = o.store.get_task("meters")
        self.assertEqual((t["state"], t["integrated"]), ("integrating", 0))
        self.assertIn("commit", t["note"])
        self.assertFalse(t["escalated"])
        o.store.set_task("meters", integrated=1)
        self.open(o, "meters")
        t = o.store.get_task("meters")
        self.assertTrue(t["escalated"])
        self.assertEqual(t["integrated"], 0)

    def test_other_errors_fall_through(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        self.feature(o, "m")
        ts = o.store.feature_tasks("m")
        self.assertFalse(triage_pr.handle(o, {"id": "m"}, ts,
                                          ForgeError("push failed"), 1.0))


class ViaDaemon(support.DaemonCase):
    def test_create_pr_no_commits_on_verify_item_closes_without_failure_count(self):
        o = self.make(floors={"daw": 0})
        o.store.add_task("verify-q", "daw", "rt-engineer", files=["q/**"],
                         feature_id="verify-q", state="integrating")
        o.store.set_task("verify-q", integrated=1)
        orig = self.forge.create_pr

        def boom(*a, **k):
            raise NOCOMMIT
        self.forge.create_pr = boom
        try:
            self.assertTrue(self.pump(
                o, lambda: o.store.get_task("verify-q")["state"] == "closed"))
        finally:
            self.forge.create_pr = orig
        self.assertEqual(o.store.events("verify-q", "pr_open_failed"), [])


if __name__ == "__main__":
    unittest.main()

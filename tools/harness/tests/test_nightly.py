"""harness/nightly.py (ADR-0061): a red nightly run on main opens one fix
task; a green one, or one seen before, opens nothing."""
import unittest

import support
from harness import nightly


class Nightly(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def run_(self, rid, conclusion, status="completed"):
        return {"id": rid, "head_sha": "a" * 40, "status": status,
                "conclusion": conclusion, "url": f"https://x/runs/{rid}"}

    def test_red_run_opens_a_queued_fix_task_with_the_excerpt(self):
        o = self.make()
        self.forge.nightly_runs = [self.run_(7, "failure")]
        self.forge.failure_excerpt = lambda sha, per_job=8: "[sanitizers]\nTSan race in ring"
        o.tick()
        t = o.store.get_task("main-red-aaaaaaaa")
        # queued, or already launched by the same tick's scheduler
        self.assertIn(t["state"], ("queued", "building"))
        self.assertEqual(t["agent"], "rt-engineer")
        self.assertIn("TSan race in ring", t["brief"])
        self.assertIn("Never weaken", t["brief"])
        self.assertIn("https://x/runs/7", t["brief"])

    def test_the_same_run_is_judged_once_and_a_green_one_opens_nothing(self):
        o = self.make()
        self.forge.nightly_runs = [self.run_(7, "failure")]
        o.tick()
        self.clock.offset += 1000
        o.tick()
        self.assertEqual(len([t for t in o.store.tasks()
                              if t["id"].startswith("main-red")]), 1)
        self.forge.nightly_runs = [self.run_(8, "success")]
        self.clock.offset += 1000
        o.tick()
        self.assertEqual(len([t for t in o.store.tasks()
                              if t["id"].startswith("main-red")]), 1)

    def test_a_run_still_in_progress_waits(self):
        o = self.make()
        self.forge.nightly_runs = [self.run_(9, None, status="in_progress")]
        o.tick()
        self.assertEqual([t for t in o.store.tasks()
                          if t["id"].startswith("main-red")], [])

    def test_polls_no_more_than_the_interval(self):
        o = self.make()
        calls = []
        self.forge.workflow_runs = lambda *a, **k: (calls.append(1), [])[1]
        o.tick()
        o.tick()
        self.assertEqual(len(calls), 1)
        self.clock.offset += o.cfg.nightly_poll_s + 1
        o.tick()
        self.assertEqual(len(calls), 2)

    def test_a_forge_error_is_an_event_not_a_crash(self):
        o = self.make()
        self.forge.fail.add("workflow_runs")
        o.tick()
        self.assertTrue(any(e["kind"] == "poll_failed"
                            for e in o.store.events("nightly")))


if __name__ == "__main__":
    unittest.main()

"""harness/nightly.py (ADR-0061): a red nightly run on main opens one fix
task; a green one, or one seen before, opens nothing."""
import os
import time
import unittest

import support
from harness import nightly, nightly_watch


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


def iso(ts):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ts))


class NightlyGuarantee(Nightly):
    def at(self, rid, conclusion, age_h, status="completed", sha="b" * 40):
        r = self.run_(rid, conclusion, status)
        r["head_sha"] = sha
        r["updated_at"] = iso(self.clock() - age_h * 3600)
        return r

    def mains(self, o):
        return [t for t in o.store.tasks() if t["id"].startswith("main-red")]

    def test_stale_verdict_dispatches_once_per_six_hours(self):
        o = self.make()
        self.forge.nightly_runs = [self.at(1, "success", 30)]
        o.tick()
        self.assertEqual(self.forge.dispatched, [("nightly.yml", "main")])
        self.assertTrue(any(e["kind"] == "nightly_dispatched"
                            for e in o.store.events("nightly")))
        self.clock.offset += 1000
        o.tick()
        self.assertEqual(len(self.forge.dispatched), 1)
        self.clock.offset += 6 * 3600
        o.tick()
        self.assertEqual(len(self.forge.dispatched), 2)

    def test_fresh_verdict_or_run_in_flight_does_not_dispatch(self):
        o = self.make()
        self.forge.nightly_runs = [self.at(1, "failure", 5)]
        o.tick()
        self.assertEqual(self.forge.dispatched, [])
        o2 = self.make()
        self.forge.nightly_runs = [self.at(3, None, 0, "queued"),
                                   self.at(2, "success", 40)]
        o2.tick()
        self.assertEqual(self.forge.dispatched, [])

    def test_cancelled_opens_no_task_and_redispatches(self):
        o = self.make()
        self.forge.nightly_runs = [self.at(5, "cancelled", 1)]
        o.tick()
        self.assertEqual(self.mains(o), [])
        self.assertEqual(len(self.forge.dispatched), 1)
        self.assertTrue(any(e["kind"] == "nightly_inconclusive"
                            for e in o.store.events("nightly")))

    def test_failure_is_a_verdict_and_no_dispatch(self):
        o = self.make()
        self.forge.nightly_runs = [self.at(6, "failure", 1)]
        o.tick()
        self.assertEqual(len(self.mains(o)), 1)
        self.assertEqual(self.forge.dispatched, [])

    def test_a_bogus_queued_task_of_a_cancelled_run_is_closed(self):
        o = self.make()
        sha = "ffa51121" + "0" * 32
        nightly.open_fix_task(o.store, sha, self.run_(4, "cancelled"), "")
        self.forge.nightly_runs = [self.at(4, "cancelled", 1, sha=sha)]
        o.tick()
        self.assertEqual(o.store.get_task("main-red-ffa51121")["state"],
                         "closed")

    def test_branch_and_pr_runs_never_count(self):
        o = self.make()
        r = self.at(8, "failure", 1)
        r["branch"] = "feature/x"
        p = self.at(9, "success", 1)
        p["event"] = "pull_request"
        self.forge.nightly_runs = [r, p]
        o.tick()
        self.assertEqual(self.mains(o), [])
        self.assertEqual(len(self.forge.dispatched), 1)

    def test_status_carries_the_last_run_for_the_dashboard(self):
        o = self.make()
        self.forge.nightly_runs = [self.at(7, "success", 2)]
        o.tick()
        st = nightly_watch.status(o.store)
        self.assertEqual(st["last"]["conclusion"], "success")
        self.assertEqual(st["last"]["sha"], "b" * 12)
        self.assertAlmostEqual(st["last"]["at"], self.clock() - 7200, delta=5)

    def test_gh_forge_filters_branch_and_event(self):
        import json
        from harness import forge
        g = forge.GhForge.__new__(forge.GhForge)
        rows = [{"databaseId": 1, "headBranch": "main", "event": "schedule"},
                {"databaseId": 2, "headBranch": "x", "event": "workflow_dispatch"},
                {"databaseId": 3, "headBranch": "main", "event": "push"}]
        g._ok = lambda argv, **k: json.dumps(rows)
        self.assertEqual([r["id"] for r in g.workflow_runs("nightly.yml")], [1])

    def test_workflow_never_cancels_main(self):
        with open(os.path.join(os.path.dirname(__file__), "..", "..", "..",
                               ".github", "workflows", "nightly.yml")) as f:
            y = f.read()
        self.assertIn("group: nightly-${{ github.ref }}", y)
        self.assertIn("cancel-in-progress: ${{ github.ref != 'refs/heads/main' }}", y)


if __name__ == "__main__":
    unittest.main()

"""harness/stall_check: a task, wake, feature or PR with no run and no
progress past the bound is a dashboard alert, never a silent wait
(2026-11-08: claim_denied every tick for hours, features in planning with
no architect, PRs open and unmerged, all unseen)."""
import json
import unittest

from harness.scheduler import HOLD
from harness.supervisor import Thresholds
import test_supervisor as ts

H = 3600.0


class Stalls(ts.Case):
    def setUp(self):
        super().setUp()
        self.th = Thresholds(interval_s=0, notify_repeat_s=600,
                             stall_alert_s=2 * H, pr_stall_s=3 * H)
        self.sv = ts.Supervisor(self.o, self.sys, th=self.th,
                                clock=self.clock)
        self.o.supervisor = self.sv

    def age(self, hours):
        self.clock.offset += hours * H

    def test_queued_task_refused_a_claim_for_hours_is_an_alert(self):
        self.o.store.add_task("f.contract", "daw", "rt-engineer",
                              files=["x/**"], feature_id="f")
        self.o.store.event("f.contract", "claim_denied",
                           json.dumps({"x/**": "architect"}))
        self.assertNotIn("stall:f.contract", self.sweep())
        self.age(2.5)
        a = self.sweep()["stall:f.contract"]
        self.assertIn("sits in queued with no agent for 2h", a["text"])
        self.assertIn("claim_denied", a["text"])
        self.assertIn("architect", a["text"])

    def test_running_held_or_terminal_tasks_are_not_stalled(self):
        s = self.o.store
        s.add_task("run", "daw", "rt-engineer", state="building")
        s.add_run(id="run-r1", task_id="run", role="engineer", name="eng-1",
                  state="building", pid=1, started=self.now(),
                  last_beat=self.now(), result_path="x", stream_path="y",
                  worktree=None)
        s.add_task("held", "daw", "rt-engineer", feature_id="g")
        s.set_task("held", not_before=HOLD)
        s.add_task("done", "daw", "rt-engineer", state="merged")
        self.age(5)
        self.assertEqual([k for k in self.sweep() if k.startswith("stall:")],
                         [])

    def test_integrating_task_counts_the_integrator_on_its_sibling(self):
        s = self.o.store
        for n in ("f0", "f1"):
            s.add_task(n, "daw", "rt-engineer", feature_id="f",
                       state="integrating")
        s.add_run(id="f0-r1", task_id="f0", role="integrator",
                  name="integrator", state="integrating", pid=1,
                  started=self.now(), last_beat=self.now(), result_path="x",
                  stream_path="y", worktree=None)
        self.age(5)
        self.assertEqual([k for k in self.sweep() if k.startswith("stall:")],
                         [])

    def test_unserved_wake_is_an_alert(self):
        self.o._ensure_wake("architect", "plan_request", "feat-x")
        self.age(3)
        ids = [k for k in self.sweep() if k.startswith("stall:wake-")]
        self.assertEqual(len(ids), 1)
        self.assertIn("architect wake", self.sweep()[ids[0]]["text"])

    def test_feature_in_planning_with_no_wake_is_rewoken_and_warned(self):
        s = self.o.store
        s.add_feature("p", "daw", branch="feat/p")
        s.set_feature("p", state="planning")
        s.set_meta("feature:p", json.dumps({"backlog_ids": ["b1"],
                                            "wants": ["adr"]}))
        self.assertEqual([k for k in self.sweep() if "p" in k], [])
        self.age(2.5)
        al = self.sweep()
        self.assertEqual(al["stall:p"]["level"], "warn")
        wakes = [w for w in s.tasks(kind="wake")
                 if w["id"].startswith("wake-architect-plan_request-p-")]
        self.assertEqual([w["state"] for w in wakes], ["queued"])
        # the pending wake now stands: no second one, alert gone
        self.age(3)
        self.assertNotIn("stall:p", self.sweep())
        self.assertEqual(len([w for w in s.tasks(kind="wake")
                              if "plan_request-p-" in w["id"]]), 1)

    def test_feature_waiting_on_a_blocked_feature_is_an_alert(self):
        s = self.o.store
        s.add_feature("dep", "daw", branch="feat/dep")
        s.set_feature("dep", state="blocked")
        s.set_meta("feature:dep", json.dumps({"backlog_ids": ["ui.x"]}))
        s.add_feature("w", "daw", branch="feat/w")
        s.set_feature("w", state="queued")
        s.set_meta("feature:w", json.dumps({"backlog_ids": [],
                                            "waits_on": "ui.x"}))
        a = self.sweep()["waits:w"]
        self.assertIn("waits on dep", a["text"])
        s.set_feature("dep", state="building")   # alive again: no alert
        self.assertNotIn("waits:w", self.sweep())

    def test_open_pr_not_merged_for_hours_is_an_alert_with_the_reason(self):
        s = self.o.store
        s.add_task("pr", "daw", "rt-engineer", state="pr_open",
                   feature_id="prf")
        s.set_feature("prf", pr=77, state="pr_open")
        s.event("pr", "feature_merge_failed", "gh pr merge: not mergeable")
        self.age(2.5)
        self.assertNotIn("prstall:prf", self.sweep())
        self.age(1)
        a = self.sweep()["prstall:prf"]
        self.assertIn("PR #77", a["text"])
        self.assertIn("feature_merge_failed", a["text"])


if __name__ == "__main__":
    unittest.main()

"""harness/stall_resolver: a stalled feature gets one principal-engineer run
(trigger, dedupe within the bound, cap, skip when a fixer is on it)."""
import unittest

import support  # noqa: F401  puts tools/ on sys.path before harness.*
from harness.supervisor import Thresholds
from test_stall_check import dead_pid
import test_supervisor as ts

H = 3600.0


class Resolver(ts.Case):
    def setUp(self):
        super().setUp()
        self.th = Thresholds(interval_s=0, notify_repeat_s=600,
                             stall_alert_s=2 * H, pr_stall_s=3 * H)
        self.sv = ts.Supervisor(self.o, self.sys, th=self.th,
                                clock=self.clock)
        self.o.supervisor = self.sv

    def age(self, hours):
        self.clock.offset += hours * H

    def stalled_feature(self, fid="f", blocked_note=None):
        s = self.o.store
        s.add_task(f"{fid}.a", "daw", "rt-engineer", files=["x/**"],
                   feature_id=fid)
        if blocked_note:
            s.add_task(f"{fid}.b", "daw", "rt-engineer", feature_id=fid,
                       state="blocked")
            s.set_task(f"{fid}.b", note=blocked_note)

    def resolvers(self):
        return [w for w in self.o.store.tasks(kind="wake")
                if w.get("event") == "stall_resolver"]

    def run_of(self, task_id, role):
        self.o.store.add_run(id=task_id + "-r9", task_id=task_id, role=role,
                             name="x-" + role, state="x", pid=dead_pid(),
                             started=self.now(), last_beat=self.now(),
                             result_path="x", stream_path="y", worktree=None)

    def test_not_before_the_bound_then_one_principal_run_with_the_brief(self):
        self.stalled_feature(blocked_note="claim x/** held by architect")
        self.sweep()
        self.assertEqual(self.resolvers(), [])
        self.age(2.5)
        al = self.sweep()
        (w,) = self.resolvers()
        self.assertEqual(w["agent"], "principal-engineer")
        self.assertEqual(w["feature_id"], "f")
        for part in ("f.a", "x/**", "Blocked sibling f.b",
                     "held by architect"):
            self.assertIn(part, w["brief"])
        self.assertIn("harness cuidando: principal-engineer (na fila)",
                      al["stall:f.a"]["text"])

    def test_console_text_names_the_running_resolver(self):
        self.stalled_feature()
        self.age(2.5)
        self.sweep()
        (w,) = self.resolvers()
        self.run_of(w["id"], "planner")
        self.assertIn("harness cuidando: x-planner",
                      self.sweep()["stall:f.a"]["text"])

    def test_not_relaunched_for_the_same_feature_within_the_bound(self):
        self.stalled_feature()
        self.age(2.5)
        self.sweep()
        self.o.store.set_task(self.resolvers()[0]["id"], state="merged")
        self.age(1)
        self.sweep()
        self.assertEqual(len(self.resolvers()), 1)
        self.age(1.5)    # 2.5 h after the first one
        self.sweep()
        self.assertEqual(len(self.resolvers()), 2)

    def test_cap_is_one_at_a_time(self):
        self.stalled_feature("f")
        self.stalled_feature("g")
        self.age(2.5)
        self.sweep()
        self.assertEqual([w["feature_id"] for w in self.resolvers()], ["f"])
        self.o.cfg.max_stall_resolvers = 2
        self.sweep()
        self.assertEqual(len(self.resolvers()), 2)

    def test_a_fixer_already_on_the_feature_means_no_resolver(self):
        self.stalled_feature()
        self.o.store.add_task("f.c", "daw", "rt-engineer", feature_id="f",
                              state="ci_fix")
        self.run_of("f.c", "fixer")
        self.age(2.5)
        self.sweep()
        self.assertEqual(self.resolvers(), [])

    def dependent_chain(self):
        """f.contract (in QA) <- f.ui (queued) <- f.tests (queued)."""
        s = self.o.store
        s.add_task("f.contract", "daw", "ui-engineer", feature_id="f",
                   state="qa")
        s.add_task("f.ui", "daw", "ui-engineer", feature_id="f",
                   depends_on=["f.contract"])
        s.add_task("f.tests", "daw", "ui-engineer", feature_id="f",
                   depends_on=["f.ui"])

    def test_waiting_on_a_dependency_with_a_live_tester_is_not_a_stall(self):
        # 2026-11-09 #448: a resolver was queued for ui.workflow.windows
        # while its contract was in QA with a tester running.
        self.dependent_chain()
        self.run_of("f.contract", "tester")
        self.age(2.5)
        self.sweep()
        self.assertEqual(self.resolvers(), [])

    def test_waiting_on_an_idle_dependency_still_gets_a_resolver(self):
        self.dependent_chain()
        self.age(2.5)
        self.sweep()
        (w,) = self.resolvers()
        self.assertIn("f.ui", w["brief"])


if __name__ == "__main__":
    unittest.main()

"""A closed slice (superseded by a re-plan, cancelled, already on main) no
longer counts for its feature: it neither stands for it nor holds its
integration. 2026-11-13: ui.record.punch_loop had .ui and .tests closed by
re-plan 1 and .ui-r1 integrating; the closed .ui was the feature's first
task and "closed" is not done, so no integrator ever launched, the PR could
never open and the feature stayed in qa, with nothing reported."""
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import scheduler
from test_daemon_unit import Base, plan
import test_supervisor as ts


def punch_loop(store, last_state="integrating"):
    """The live shape: two closed slices first, the re-planned one last."""
    store.add_task("f.ui", "daw", "ui-engineer", state="closed",
                   feature_id="f", files=["ui/x/**"])
    store.add_task("f.tests", "daw", "tester", state="closed",
                   feature_id="f", depends_on=["f.ui"], files=["ui/t/**"])
    store.add_task("f.ui-r1", "daw", "ui-engineer", state=last_state,
                   feature_id="f", files=["ui/x/**"])


class SchedulerTests(Base):
    def test_closed_first_sibling_does_not_hold_the_integrator(self):
        punch_loop(self.s)
        p = plan(self.s)
        self.assertIn(("f.ui-r1", "integrator"),
                      [(x.task_id, x.kind) for x in p.launches])

    def test_closed_first_sibling_does_not_hold_the_fixer(self):
        punch_loop(self.s, last_state="ci_fix")
        p = plan(self.s)
        self.assertIn(("f.ui-r1", "fixer"),
                      [(x.task_id, x.kind) for x in p.launches])

    def test_live_unfinished_sibling_still_holds_the_integrator(self):
        punch_loop(self.s)
        self.s.add_task("f.docs-r1", "daw", "docs", state="building",
                        feature_id="f", files=["docs/f/**"])
        p = plan(self.s, runs=[{"task_id": "f.docs-r1", "role": "engineer",
                                "working": True, "building": False,
                                "name": "eng-9"}])
        self.assertNotIn("integrator", [x.kind for x in p.launches])

    def test_one_integrator_per_feature_among_live_tasks(self):
        punch_loop(self.s)
        self.s.add_task("f.tests-r1", "daw", "tester", state="integrating",
                        feature_id="f", files=["ui/t/**"])
        p = plan(self.s, cfg=scheduler.Config(max_integrators=4))
        self.assertEqual(["f.ui-r1"], [x.task_id for x in p.launches
                                       if x.kind == "integrator"])


class DaemonTests(ts.Case):
    def test_ready_feature_ignores_closed_siblings(self):
        s = self.o.store
        s.add_feature("f", "daw", branch="feat/f")
        punch_loop(s)
        s.set_task("f.ui-r1", integrated=1)
        ready = [(f["id"], [t["id"] for t in tasks])
                 for f, tasks in self.o._ready_features()]
        self.assertEqual([("f", ["f.ui-r1"])], ready)

    def test_feature_state_follows_live_tasks(self):
        s = self.o.store
        s.add_feature("f", "daw", branch="feat/f")
        s.set_feature("f", state="qa")
        punch_loop(s)
        self.o.flow.sync_states()
        self.assertEqual("integrating", s.get_feature("f")["state"])

    def test_stall_text_never_names_a_closed_sibling(self):
        from harness import stall_check
        s = self.o.store
        s.add_feature("f", "daw", branch="feat/f")
        punch_loop(s)
        waiting = stall_check._waiting_siblings(self.sv, s.get_task("f.ui-r1"))
        self.assertEqual([], waiting)


if __name__ == "__main__":
    unittest.main()

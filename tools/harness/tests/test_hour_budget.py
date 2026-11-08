"""The hourly run limit counts work runs, not planner wakes (2026-11-08:
28 one-minute lead/architect runs used up the hour while 3 engineers
worked, and every build pool showed 'hourly run limit')."""
import os
import shutil
import tempfile
import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import scheduler, status
from harness.store import WORK_ROLES, Store


class HourBudget(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.s = Store(os.path.join(self.tmp, "state.json"))

    def run_of(self, i, role, started):
        self.s.add_run(id=f"r{i}", task_id=f"t{i}", role=role,
                       name=f"{role}-{i}", started=started)

    def test_planner_runs_do_not_count(self):
        for i in range(28):
            self.run_of(i, "planner", 1000.0)
        for i in range(28, 31):
            self.run_of(i, "engineer", 1000.0)
        self.assertEqual(self.s.runs_started_since(0), 31)
        self.assertEqual(self.s.runs_started_since(0, WORK_ROLES), 3)

    def test_every_work_role_counts(self):
        for i, role in enumerate(WORK_ROLES):
            self.run_of(i, role, 1000.0)
        self.assertEqual(self.s.runs_started_since(0, WORK_ROLES),
                         len(WORK_ROLES))
        self.assertEqual(self.s.runs_started_since(2000, WORK_ROLES), 0)


class SplitBudgets(unittest.TestCase):
    """Work and planner runs are limited separately in scheduler.plan."""
    NOW = 1000.0

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.s = Store(os.path.join(self.tmp, "s.json"),
                       clock=lambda: self.NOW)
        self.s.add_task("e1", "daw", "rt-engineer", files=["daw/a/**"])
        self.s.add_task("w1", "daw", "lead", kind="wake")

    def plan(self, **kw):
        return scheduler.plan(self.s.tasks(), [], scheduler.Config(cap=10),
                              self.NOW, **kw)

    def kinds(self, p):
        return sorted(ln.kind for ln in p.launches)

    def test_spent_planner_budget_does_not_hold_builders(self):
        p = self.plan(hour_budget=5, planner_budget=0)
        self.assertEqual(self.kinds(p), ["engineer"])
        self.assertEqual(p.holds, {"planner": "hourly run limit"})

    def test_spent_work_budget_does_not_hold_planners(self):
        p = self.plan(hour_budget=0, planner_budget=5)
        self.assertEqual(self.kinds(p), ["planner"])
        self.assertEqual(p.holds, {"work": "hourly run limit"})
        self.assertEqual(p.gaps.get("daw"), "hourly run limit")

    def test_planner_budget_caps_planner_launches(self):
        self.s.add_task("w2", "daw", "architect", kind="wake")
        p = self.plan(hour_budget=5, planner_budget=1)
        self.assertEqual(self.kinds(p).count("planner"), 1)

    def test_default_planner_limit_and_status(self):
        cfg = scheduler.Config()
        self.assertEqual(cfg.max_planner_runs_per_hour, 20)
        d = status.build(self.s, cfg, self.NOW, [], {}, "live", False, False,
                         0, (), 7, 3, {"work": "hourly run limit"})
        self.assertEqual((d["runs_last_hour"], d["max_runs_per_hour"]),
                         (7, 60))
        self.assertEqual((d["planner_runs_last_hour"],
                          d["max_planner_runs_per_hour"]), (3, 20))
        self.assertEqual(d["budget_holds"], {"work": "hourly run limit"})


if __name__ == "__main__":
    unittest.main()

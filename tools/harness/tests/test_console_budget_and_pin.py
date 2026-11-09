"""Owner 2026-11-08: console replies spend no planner budget (they ate 5 of
20 planner runs an hour), and the stall resolver runs on opus, not fable;
the escalation principal stays fable."""
import os
import sys
import unittest

import support  # noqa: F401  puts tools/ on sys.path before harness.*
from harness import launcher, scheduler

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
import dashboard_board  # noqa: E402


def wake(id_, agent):
    return {"id": id_, "kind": "wake", "state": "queued", "agent": agent,
            "track": None, "feature_id": None, "not_before": 0, "files": [],
            "priority": 0, "created": 0, "attempts": 0}


class Budget(unittest.TestCase):
    def plan(self, tasks, planner_budget):
        cfg = scheduler.Config()
        return scheduler.plan(tasks, [], cfg, 1000.0, planner_budget=planner_budget)

    def test_console_launches_with_the_planner_budget_spent(self):
        p = self.plan([wake("w-console", "console"), wake("w-arch", "architect")], 0)
        ids = [ln.task_id for ln in p.launches]
        self.assertIn("w-console", ids)
        self.assertNotIn("w-arch", ids)

    def test_console_does_not_spend_the_budget(self):
        p = self.plan([wake("w-console", "console"), wake("w-arch", "architect")], 1)
        self.assertEqual(sorted(ln.task_id for ln in p.launches),
                         ["w-arch", "w-console"])


class Model(unittest.TestCase):
    def test_escalation_principal_stays_fable(self):
        self.assertEqual(launcher.model_for("principal-engineer", None), "fable")

    def test_a_tier_model_does_not_override_the_principal(self):
        self.assertEqual(launcher.model_for("principal-engineer", "haiku"), "fable")

    def test_a_pinned_model_wins(self):
        self.assertEqual(launcher.model_for("principal-engineer",
                                            launcher.Pinned("opus")), "opus")
        self.assertEqual(launcher.model_args("principal-engineer",
                                             launcher.Pinned("opus")),
                         ["--model", "opus"])


class Labels(unittest.TestCase):
    def test_the_run_agent_names_the_role(self):
        f = dashboard_board.run_role_pt
        self.assertEqual(f({"role": "planner", "agent": "principal-engineer"}),
                         "Engenheiro principal")
        self.assertEqual(f({"role": "engineer", "agent": "plugin-engineer"}),
                         "Engenheiro de plugins")
        self.assertEqual(f({"role": "planner", "agent": "architect"}), "Arquiteto")
        self.assertEqual(f({"role": "tester"}), "Testes (QA)")
        self.assertEqual(f({"role": "tester", "agent": "ui-engineer"}), "Testes (QA)")
        self.assertEqual(f({"role": "integrator", "agent": "rt-engineer"}), "Integrador")


if __name__ == "__main__":
    unittest.main()

"""The ready-to-PR budget counts features and never starves a sibling."""
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import scheduler
from test_daemon_unit import Base, plan


class ReadyBudgetTests(Base):
    def test_sibling_of_a_ready_feature_launches_with_budget_full(self):
        # Two integrating slices of one feature are one ready feature; the
        # third slice must launch although the budget is full, or the
        # feature (and every pool) deadlocks.
        cfg = scheduler.Config(max_ready_unpr=2)
        for tid, fid in (("a1", "fa"), ("a2", "fa"), ("b1", "fb")):
            self.s.add_task(tid, "daw", "rt-engineer", state="integrating",
                            feature_id=fid)
        self.s.add_task("a3", "daw", "rt-engineer", feature_id="fa")
        self.s.add_task("c1", "daw", "rt-engineer", feature_id="fc")
        p = plan(self.s, cfg=cfg)
        launched = [l.task_id for l in p.launches if l.kind == "engineer"]
        self.assertIn("a3", launched)
        self.assertNotIn("c1", launched)


if __name__ == "__main__":
    unittest.main()

"""Open PRs are the brake on new work (owner 2026-11-08): at max_open_prs
no queued task starts, while fixes of those PRs still launch."""
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import scheduler
from test_daemon_unit import Base, plan


class OpenPrBrake(Base):
    def setUp(self):
        super().setUp()
        for i in range(2):
            self.s.add_task(f"p{i}", "daw", "rt-engineer", state="pr_open",
                            feature_id=f"f{i}")
        self.s.add_task("new", "daw", "rt-engineer", feature_id="fn")

    def test_new_work_waits_while_open_prs_are_at_the_limit(self):
        p = plan(self.s, cfg=scheduler.Config(max_open_prs=2))
        self.assertNotIn("new", [l.task_id for l in p.launches])
        self.assertEqual(p.gaps.get("daw"), "2 open PRs: merging first")

    def test_new_work_starts_below_the_limit(self):
        p = plan(self.s, cfg=scheduler.Config(max_open_prs=3))
        self.assertIn("new", [l.task_id for l in p.launches])

    def test_a_red_pr_still_gets_its_fixer(self):
        self.s.set_task("p0", state="ci_fix")
        p = plan(self.s, cfg=scheduler.Config(max_open_prs=2))
        self.assertIn("p0", [l.task_id for l in p.launches])


if __name__ == "__main__":
    unittest.main()

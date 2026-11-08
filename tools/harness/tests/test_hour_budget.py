"""The hourly run limit counts work runs, not planner wakes (2026-11-08:
28 one-minute lead/architect runs used up the hour while 3 engineers
worked, and every build pool showed 'hourly run limit')."""
import os
import shutil
import tempfile
import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
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


if __name__ == "__main__":
    unittest.main()

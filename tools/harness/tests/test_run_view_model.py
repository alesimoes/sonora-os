"""Status runs report the model the launcher chose and who launched them."""
import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import run_view


class RunViewModel(unittest.TestCase):
    def test_model_is_the_recorded_one_never_a_default(self):
        self.assertEqual(run_view.run_view({"model": "fable"}, None)["model"],
                         "fable")
        self.assertIsNone(run_view.run_view({}, None)["model"])

    def test_wake_task_is_daemon_launched_and_titled_by_feature(self):
        t = {"id": "wake-architect-plan_request-x-1", "kind": "wake",
             "feature_id": "dash.x", "brief": "event plan_request: x"}
        v = run_view.run_view({}, t)
        self.assertEqual((v["launched_by"], v["title"]), ("daemon", "dash.x"))

    def test_other_tasks_are_coordinator_launched(self):
        t = {"id": "t1", "kind": "work", "brief": "# Fazer X"}
        v = run_view.run_view({}, t)
        self.assertEqual((v["launched_by"], v["title"]),
                         ("coordenador", "Fazer X"))


if __name__ == "__main__":
    unittest.main()

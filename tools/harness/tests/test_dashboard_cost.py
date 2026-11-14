"""The day's spend shown on the console (dashboard_cost.py)."""
import json
import os
import shutil
import tempfile
import time
import unittest

import support  # noqa: F401  (sets sys.path)
import dashboard_cost as C


def result(total, **by_model):
    return json.dumps({"type": "result", "total_cost_usd": total, "modelUsage": {
        f"claude-{m}-5-5": {"costUSD": usd} for m, usd in by_model.items()}},
        separators=(",", ":"))     # the CLI writes compact JSON


class Cost(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        C._cache.clear()

    def run_log(self, name, line, age_s=0):
        d = os.path.join(self.tmp, name)
        os.makedirs(d)
        p = os.path.join(d, "stream.log")
        with open(p, "w", encoding="utf-8") as f:
            f.write('{"type":"assistant"}\n' + line + "\n")
        t = time.time() - age_s
        os.utime(p, (t, t))

    def test_sums_the_day_by_model_and_names_the_dearest_run(self):
        self.run_log("a-r1", result(5.0, opus=5.0))
        self.run_log("b-r2", result(2.5, sonnet=2.0, haiku=0.5))
        got = C.read_cost(self.tmp)
        self.assertEqual(got["usd"], 7.5)
        self.assertEqual(got["by_model"], {"opus": 5.0, "sonnet": 2.0, "haiku": 0.5})
        self.assertEqual(got["top"], {"run": "a", "usd": 5.0})

    def test_runs_older_than_a_day_and_unfinished_runs_do_not_count(self):
        self.run_log("old-r1", result(9.0, opus=9.0), age_s=C.WINDOW_S + 60)
        self.run_log("busy-r2", '{"type":"assistant"}')
        self.assertEqual(C.read_cost(self.tmp)["usd"], 0.0)

    def test_a_missing_directory_is_none(self):
        self.assertIsNone(C.read_cost(os.path.join(self.tmp, "nope")))


if __name__ == "__main__":
    unittest.main()

"""tools/micro_summary.py: the table the micro build writes to the job summary."""
import os
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
import micro_summary as ms

JUNIT = """<testsuite><testcase name="a" time="0.5" status="run"/>
<testcase name="b" time="2" status="fail"><failure/></testcase></testsuite>"""


class Summary(unittest.TestCase):
    def test_no_plan_says_so(self):
        self.assertIn("No plan", ms.render("T", [], None, []))

    def test_table_lists_files_targets_and_failures_first(self):
        plan = {"scope": "targets", "targets": ["t1"], "tests": ["a", "b"],
                "why": {"common/x.cpp": "owned by t1"}}
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "ctest.xml")
            with open(p, "w") as f:
                f.write(JUNIT)
            res = ms.read_results(p)
        out = ms.render("Build + test", ["common/x.cpp"], plan, res)
        self.assertIn("1 passed, 1 failed", out)
        self.assertIn("| `common/x.cpp` | owned by t1 |", out)
        self.assertLess(out.index("| b | FAIL"), out.index("| a | pass"))

    def test_a_missing_junit_file_is_an_empty_result(self):
        self.assertEqual(ms.read_results("/no/such/ctest.xml"), [])


if __name__ == "__main__":
    unittest.main()

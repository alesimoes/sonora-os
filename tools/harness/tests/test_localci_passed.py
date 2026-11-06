import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from harness.localci import passed  # noqa: E402


class PassedTest(unittest.TestCase):
    def test_needs_the_script_verdict(self):
        self.assertTrue(passed(0, "local-ci: tree\nok   tree (1 s)\nlocal-ci: all passed\n"))
        self.assertTrue(passed(0, "local-ci: nothing to build or test\n"))
        # killed mid-run: the wrapper wrote rc 0, there is a FAIL line and
        # no verdict (2026-11-05, a red batch taken as green)
        self.assertFalse(passed(0, "local-ci: tree x\nFAIL tree (9 s): l\nok   x (1 s)\n"))
        self.assertFalse(passed(0, "local-ci: tree\nok   tree (1 s)\n"))
        self.assertFalse(passed(1, "local-ci: all passed\n"))


if __name__ == "__main__":
    unittest.main()

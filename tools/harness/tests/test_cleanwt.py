import subprocess
import unittest

import support  # noqa: F401
from harness import cleanwt


def cp(rc):
    return subprocess.CompletedProcess([], rc, "", "")


class CleanTest(unittest.TestCase):
    def run_with(self, rcs):
        calls = []

        def git(args, cwd):
            calls.append(args)
            return cp(rcs[min(len(calls) - 1, len(rcs) - 1)])
        r = cleanwt.clean(git, "wt", ("a",), sleep=lambda s: None)
        return r, calls

    def test_retries_until_stable(self):
        r, calls = self.run_with([1, 1, 0])
        self.assertEqual(r.returncode, 0)
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[0], ["clean", "-fdxq", "-e", "a"])

    def test_persistent_failure_is_reported(self):
        r, calls = self.run_with([1])
        self.assertEqual(r.returncode, 1)
        self.assertEqual(len(calls), 1 + cleanwt.RETRIES)


if __name__ == "__main__":
    unittest.main()

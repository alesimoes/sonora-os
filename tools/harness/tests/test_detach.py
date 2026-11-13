"""harness/detach.py: a build job's runner starts with no console, in its own
process group, out of the caller's Windows job when the job allows it, with
no pipe back to the caller (tools/build-job.sh)."""
import os
import subprocess
import sys
import tempfile
import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import detach


class FakePopen:
    def __init__(self, refuse_breakaway=False):
        self.calls, self.refuse = [], refuse_breakaway

    def __call__(self, argv, **kw):
        self.calls.append(kw)
        if self.refuse and kw.get("creationflags", 0) & detach.BREAKAWAY:
            raise PermissionError(5, "Access is denied")
        return type("P", (), {"pid": 4242})()


class Spawn(unittest.TestCase):
    def setUp(self):
        fd, self.out = tempfile.mkstemp(prefix="detach-")
        os.close(fd)
        self.addCleanup(os.remove, self.out)

    def test_windows_leaves_the_job_with_no_console(self):
        p = FakePopen()
        self.assertEqual(detach.spawn(["x"], ".", self.out, popen=p, nt=True).pid, 4242)
        flags = p.calls[0]["creationflags"]
        for f in (detach.BREAKAWAY, detach.NEW_GROUP, detach.NO_WINDOW):
            self.assertTrue(flags & f)
        self.assertIs(p.calls[0]["stdin"], subprocess.DEVNULL)

    def test_job_that_refuses_breakaway_still_starts_it(self):
        p = FakePopen(refuse_breakaway=True)
        detach.spawn(["x"], ".", self.out, popen=p, nt=True)
        self.assertEqual(len(p.calls), 2)
        self.assertFalse(p.calls[1]["creationflags"] & detach.BREAKAWAY)

    def test_posix_gets_its_own_session(self):
        p = FakePopen()
        detach.spawn(["x"], ".", self.out, popen=p, nt=False)
        self.assertTrue(p.calls[0]["start_new_session"])

    def test_real_child_outlives_the_call_and_writes_its_file(self):
        code = "import time; time.sleep(0.3); print('child done')"
        pid = detach.spawn([sys.executable, "-c", code], ".", self.out).pid
        self.assertGreater(pid, 0)
        for _ in range(100):
            with open(self.out) as f:
                if "child done" in f.read():
                    break
            __import__("time").sleep(0.1)
        with open(self.out) as f:
            self.assertIn("child done", f.read())


if __name__ == "__main__":
    unittest.main()

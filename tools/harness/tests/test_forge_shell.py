"""The forge runs repo scripts through Git's bash, never a bare "sh" (the
Windows daemon has no Git usr/bin on PATH: 'sh: [WinError 2]', 2026-11-07)."""
import subprocess
import tempfile
import unittest
import os

import support  # noqa: F401  (sets sys.path)
from harness import forge, scheduler, shell


class Recorder:
    def __init__(self):
        self.calls = []

    def __call__(self, argv, cwd=None, timeout=120):
        self.calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, "", "")


class ForgeUsesBash(unittest.TestCase):
    def test_enqueue_and_pre_pr_use_the_shared_bash(self):
        rec = Recorder()
        g = forge.GhForge(".", runner=rec)
        g.enqueue(5)
        wt = tempfile.mkdtemp()
        os.makedirs(os.path.join(wt, "tools"))
        open(os.path.join(wt, "tools", "pre-pr.sh"), "w").close()
        g.pre_pr(wt)
        self.assertEqual([c[0] for c in rec.calls], [shell.bash()] * 2)
        self.assertNotIn("sh", [c[0] for c in rec.calls])

    def test_scheduler_has_no_private_copy(self):
        self.assertFalse(hasattr(scheduler, "_bash"))


if __name__ == "__main__":
    unittest.main()

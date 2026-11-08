"""GhForge.branch_tip reads the LOCAL branch head: agents' sub-branches are
worktrees of the daemon's repository and a hand fix is rarely pushed."""
import os
import subprocess
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import forge


def git(cwd, *args, env=None):
    e = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
             GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t", **(env or {}))
    return subprocess.run(["git", *args], cwd=cwd, env=e, check=True,
                          capture_output=True, text=True).stdout


class BranchTip(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="tip-")
        git(self.repo, "init", "-q", "-b", "main")
        open(os.path.join(self.repo, "a"), "w").close()
        git(self.repo, "add", "a")
        git(self.repo, "commit", "-q", "-m", "first",
            env={"GIT_COMMITTER_DATE": "2026-11-08T20:00:00+00:00"})
        git(self.repo, "branch", "sub/x/docs")
        self.g = forge.GhForge(self.repo)

    def test_tip_of_a_local_branch_with_its_commit_time(self):
        git(self.repo, "checkout", "-q", "sub/x/docs")
        open(os.path.join(self.repo, "b"), "w").close()
        git(self.repo, "add", "b")
        git(self.repo, "commit", "-q", "-m", "docs: one-line pointer",
            env={"GIT_COMMITTER_DATE": "2026-11-08T21:00:00+00:00"})
        tip = self.g.branch_tip("sub/x/docs")
        self.assertEqual(tip["sha"], git(self.repo, "rev-parse", "HEAD").strip())
        self.assertEqual(tip["subject"], "docs: one-line pointer")
        self.assertEqual(tip["ts"], 1794171600.0)

    def test_unknown_branch_is_none(self):
        self.assertIsNone(self.g.branch_tip("sub/x/nothing"))


if __name__ == "__main__":
    unittest.main()

"""GhForge.branch_tip: the local branch head counts (agents' sub-branches
are worktrees of the daemon's repository and a hand fix is rarely pushed),
unless origin is strictly ahead: a stall resolver works in a worktree of its
own and can only push (plugins.offline.ui, 2026-11-09). sync_local then
fast-forwards the local branch, never over an agent's leftovers."""
import os
import shutil
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


def commit(cwd, name, msg, date="2026-11-08T21:00:00+00:00"):
    open(os.path.join(cwd, name), "w").close()
    git(cwd, "add", name)
    git(cwd, "commit", "-q", "-m", msg, env={"GIT_COMMITTER_DATE": date})
    return git(cwd, "rev-parse", "HEAD").strip()


class BranchTip(unittest.TestCase):
    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="tip-")
        self.addCleanup(shutil.rmtree, self.repo, True)
        git(self.repo, "init", "-q", "-b", "main")
        commit(self.repo, "a", "first", "2026-11-08T20:00:00+00:00")
        git(self.repo, "branch", "sub/x/docs")
        self.g = forge.GhForge(self.repo)

    def test_tip_of_a_local_branch_with_its_commit_time(self):
        git(self.repo, "checkout", "-q", "sub/x/docs")
        sha = commit(self.repo, "b", "docs: one-line pointer")
        tip = self.g.branch_tip("sub/x/docs")
        self.assertEqual(tip["sha"], sha)
        self.assertEqual(tip["subject"], "docs: one-line pointer")
        self.assertEqual(tip["ts"], 1794171600.0)
        self.assertEqual(tip["ref"], "local")

    def test_unknown_branch_is_none(self):
        self.assertIsNone(self.g.branch_tip("sub/x/nothing"))


class ChangedFiles(unittest.TestCase):
    """The paths a slice changed since the branch it started from: what a
    scope triage widens the claim with (daw.model.session_mgmt.ui)."""

    def setUp(self):
        self.repo = tempfile.mkdtemp(prefix="diff-")
        self.addCleanup(shutil.rmtree, self.repo, True)
        git(self.repo, "init", "-q", "-b", "main")
        commit(self.repo, "a", "first")
        git(self.repo, "checkout", "-q", "-b", "sub/f/model")
        commit(self.repo, "m", "model")
        git(self.repo, "checkout", "-q", "-b", "sub/f/ui")
        os.makedirs(os.path.join(self.repo, "ui"))
        commit(self.repo, "ui/v.qml", "ui")
        git(self.repo, "checkout", "-q", "main")
        commit(self.repo, "later", "main moved on")
        self.g = forge.GhForge(self.repo)

    def test_diff_since_the_start_branch(self):
        self.assertEqual(self.g.changed_files("sub/f/model", "sub/f/ui"),
                         ["ui/v.qml"])

    def test_missing_start_branch_falls_back_to_main(self):
        self.assertEqual(self.g.changed_files("feat/f", "sub/f/ui"),
                         ["m", "ui/v.qml"])

    def test_unknown_branch_is_none(self):
        self.assertIsNone(self.g.changed_files("main", "sub/f/none"))


class PushedFix(unittest.TestCase):
    """An origin, the daemon's clone with the sub-branch checked out in an
    agent worktree, and a resolver's clone that pushes a fix."""

    def setUp(self):
        top = tempfile.mkdtemp(prefix="tip-")
        self.addCleanup(shutil.rmtree, top, True)
        self.origin = os.path.join(top, "origin.git")
        git(top, "init", "-q", "--bare", "-b", "main", self.origin)
        self.root = os.path.join(top, "daemon")
        git(top, "clone", "-q", self.origin, self.root)
        commit(self.root, "a", "first", "2026-11-08T20:00:00+00:00")
        git(self.root, "push", "-q", "origin", "HEAD:main",
            "HEAD:refs/heads/sub/f/ui")
        git(self.root, "fetch", "-q", "origin")
        self.wt = os.path.join(top, "wt-ui")
        git(self.root, "worktree", "add", "-q", "-b", "sub/f/ui", self.wt,
            "origin/sub/f/ui")
        self.blocked_sha = git(self.wt, "rev-parse", "HEAD").strip()
        res = os.path.join(top, "resolver")
        git(top, "clone", "-q", "-b", "sub/f/ui", self.origin, res)
        self.fix = commit(res, "ui", "ui: the menu, by hand")
        git(res, "push", "-q", "origin", "sub/f/ui")
        self.g = forge.GhForge(self.root)

    def test_origin_strictly_ahead_is_the_tip(self):
        tip = self.g.branch_tip("sub/f/ui")
        self.assertEqual((tip["sha"], tip["ref"]), (self.fix, "origin"))

    def test_local_commits_of_its_own_win_over_origin(self):
        mine = commit(self.wt, "wip", "agent's unpushed work")
        tip = self.g.branch_tip("sub/f/ui")
        self.assertEqual((tip["sha"], tip["ref"]), (mine, "local"))

    def test_sync_fast_forwards_the_clean_agent_worktree(self):
        self.g.branch_tip("sub/f/ui")
        ok, why = self.g.sync_local("sub/f/ui")
        self.assertTrue(ok, why)
        self.assertEqual(git(self.wt, "rev-parse", "HEAD").strip(), self.fix)
        self.assertTrue(os.path.exists(os.path.join(self.wt, "ui")))

    def test_sync_leaves_a_dirty_worktree_alone(self):
        self.g.branch_tip("sub/f/ui")
        with open(os.path.join(self.wt, "a"), "w") as f:
            f.write("an agent's leftover edit")
        ok, why = self.g.sync_local("sub/f/ui")
        self.assertFalse(ok)
        self.assertIn("uncommitted", why)
        self.assertEqual(git(self.wt, "rev-parse", "HEAD").strip(),
                         self.blocked_sha)

    def test_sync_refuses_a_diverged_local_branch(self):
        self.g.branch_tip("sub/f/ui")
        mine = commit(self.wt, "wip", "agent's unpushed work")
        ok, why = self.g.sync_local("sub/f/ui")
        self.assertFalse(ok)
        self.assertEqual(git(self.wt, "rev-parse", "HEAD").strip(), mine)

    def test_sync_moves_a_branch_no_worktree_has_checked_out(self):
        git(self.wt, "checkout", "-q", "--detach")
        self.g.branch_tip("sub/f/ui")
        ok, why = self.g.sync_local("sub/f/ui")
        self.assertTrue(ok, why)
        self.assertEqual(git(self.root, "rev-parse", "sub/f/ui").strip(),
                         self.fix)


if __name__ == "__main__":
    unittest.main()

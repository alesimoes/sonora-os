"""The daemon's push of a feature branch against a branch someone else
already pushed (integrator-push-race, #500-#503): real git, local bare
origin, so the non-fast-forward is the real one."""
import os
import subprocess
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import forge, forge_push

GIT_ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")


def git(cwd, *args):
    r = subprocess.run(["git", *args], cwd=cwd, env=GIT_ENV, text=True,
                       capture_output=True)
    assert r.returncode == 0, (args, r.stderr)
    return r.stdout.strip()


def commit_file(cwd, name, text, msg):
    with open(os.path.join(cwd, name), "w") as f:
        f.write(text)
    git(cwd, "add", name)
    git(cwd, "commit", "-m", msg)


class PushRace(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.origin = os.path.join(t, "origin.git")
        git(t, "init", "--bare", "-b", "main", self.origin)
        self.seed = os.path.join(t, "seed")
        git(t, "clone", self.origin, self.seed)
        commit_file(self.seed, "a.txt", "1\n2\n3\n", "base")
        git(self.seed, "push", "origin", "HEAD:main")
        # the integrator's stray push, from the old base
        git(self.seed, "checkout", "-b", "feat/x")
        commit_file(self.seed, "feature.txt", "feature\n", "feature")
        git(self.seed, "push", "origin", "feat/x")
        # main moves on; the daemon's worktree rebases onto it
        git(self.seed, "checkout", "main")
        commit_file(self.seed, "other.txt", "o\n", "main moved")
        git(self.seed, "push", "origin", "main")
        self.wt = os.path.join(t, "daemon")
        git(t, "clone", self.origin, self.wt)
        git(self.wt, "checkout", "-b", "feat/x", "origin/feat/x")
        self.forge = forge.GhForge(self.wt, runner=forge.run)

    def tearDown(self):
        self.tmp.cleanup()

    def remote_head(self):
        return git(self.origin, "rev-parse", "feat/x")

    def test_same_patch_after_rebase_is_adopted_not_pushed(self):
        git(self.wt, "fetch", "origin", "main")
        git(self.wt, "rebase", "origin/main")
        before = self.remote_head()
        self.assertEqual(self.forge.push(self.wt, "feat/x"), forge_push.SAME)
        self.assertEqual(self.remote_head(), before)

    def test_different_patch_replaces_remote_with_lease(self):
        git(self.wt, "fetch", "origin", "main")
        git(self.wt, "rebase", "origin/main")
        commit_file(self.wt, "more.txt", "more\n", "daemon work")
        self.assertEqual(self.forge.push(self.wt, "feat/x"),
                         forge_push.PUSHED)
        self.assertEqual(self.remote_head(), git(self.wt, "rev-parse", "HEAD"))

    def test_new_branch_is_a_plain_push(self):
        git(self.wt, "checkout", "-b", "feat/new", "origin/main")
        commit_file(self.wt, "n.txt", "n\n", "new")
        self.assertEqual(self.forge.push(self.wt, "feat/new"),
                         forge_push.PUSHED)
        self.assertEqual(git(self.origin, "rev-parse", "feat/new"),
                         git(self.wt, "rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()

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

    def test_remote_ahead_is_merged_never_forced(self):
        # someone pushes a fix to the branch after the daemon's copy
        other = os.path.join(self.tmp.name, "other")
        git(self.tmp.name, "clone", self.origin, other)
        git(other, "checkout", "feat/x")
        commit_file(other, "theirs.txt", "t\n", "their fix")
        git(other, "push", "origin", "feat/x")
        theirs = git(other, "rev-parse", "HEAD")
        git(self.wt, "fetch", "origin", "main")
        git(self.wt, "rebase", "origin/main")
        commit_file(self.wt, "more.txt", "more\n", "daemon work")
        self.assertEqual(self.forge.push(self.wt, "feat/x"),
                         forge_push.PUSHED)
        head = self.remote_head()
        self.assertEqual(head, git(self.wt, "rev-parse", "HEAD"))
        git(self.origin, "merge-base", "--is-ancestor", theirs, head)
        tree = git(self.origin, "ls-tree", "--name-only", head)
        self.assertIn("theirs.txt", tree)
        self.assertIn("more.txt", tree)

    def test_conflicting_remote_raises_and_leaves_remote_alone(self):
        other = os.path.join(self.tmp.name, "other")
        git(self.tmp.name, "clone", self.origin, other)
        git(other, "checkout", "feat/x")
        commit_file(other, "feature.txt", "theirs\n", "their edit")
        git(other, "push", "origin", "feat/x")
        before = self.remote_head()
        commit_file(self.wt, "feature.txt", "ours\n", "our edit")
        with self.assertRaises(Exception):
            self.forge.push(self.wt, "feat/x")
        self.assertEqual(self.remote_head(), before)
        self.assertEqual(git(self.wt, "status", "--porcelain"), "")

    def _remote_edits(self, files):
        """The remote copy of feat/x gains `files` ({name: text|None})."""
        other = os.path.join(self.tmp.name, "other")
        git(self.tmp.name, "clone", self.origin, other)
        git(other, "checkout", "feat/x")
        os.makedirs(os.path.join(other, "docs", "images"), exist_ok=True)
        for name, text in files.items():
            with open(os.path.join(other, name), "w") as f:
                f.write(text)
            git(other, "add", "-f", name)
        git(other, "commit", "-m", "theirs")
        git(other, "push", "origin", "feat/x")

    def _local_drops_png(self):
        png = "docs/images/sonora-ui-a.png"
        os.makedirs(os.path.join(self.wt, "docs", "images"), exist_ok=True)
        with open(os.path.join(self.wt, png), "w") as f:
            f.write("v0")
        git(self.wt, "add", "-f", png)
        git(self.wt, "commit", "-m", "png")
        git(self.wt, "push", "origin", "feat/x")  # shared ancestor
        git(self.wt, "rm", "-q", png)
        git(self.wt, "commit", "-m", "untrack png (#724)")
        return png

    def test_screenshot_modify_delete_is_resolved_by_deleting(self):
        png = "docs/images/sonora-ui-a.png"
        self._local_drops_png()
        self._remote_edits({png: "v1"})
        self.assertEqual(self.forge.push(self.wt, "feat/x"),
                         forge_push.PUSHED)
        tree = git(self.origin, "ls-tree", "-r", "--name-only", "feat/x")
        self.assertNotIn(png, tree)
        self.assertEqual(git(self.wt, "status", "--porcelain"), "")

    def test_other_conflict_next_to_screenshot_is_a_conflict_with_paths(self):
        png = "docs/images/sonora-ui-a.png"
        self._local_drops_png()
        self._remote_edits({png: "v1", "feature.txt": "theirs\n"})
        commit_file(self.wt, "feature.txt", "ours\n", "our edit")
        before = self.remote_head()
        with self.assertRaises(forge.ForgeConflict) as c:
            self.forge.push(self.wt, "feat/x")
        self.assertEqual(c.exception.paths, ["feature.txt"])
        self.assertIn("feature.txt", str(c.exception))
        self.assertEqual(self.remote_head(), before)
        self.assertEqual(git(self.wt, "status", "--porcelain"), "")

    def test_error_text_keeps_stdout(self):
        class R:
            returncode, stderr, stdout = 1, "", "CONFLICT (content): x.cpp"
        g = forge.GhForge(".", runner=lambda a, c, t: R())
        with self.assertRaises(forge.ForgeError) as c:
            g._ok(["git", "merge", "X"])
        self.assertIn("CONFLICT (content): x.cpp", str(c.exception))

    def test_no_force_flag_is_ever_used(self):
        argvs = []

        def ok(argv, cwd, timeout):
            argvs.append(argv)
            if argv[:2] == ["git", "push"] and len(argvs) < 8:
                raise RuntimeError("rejected non-fast-forward")
            return ""
        try:
            forge_push.push_feature_branch(ok, ".", "b")
        except Exception:
            pass
        pushes = [a for a in argvs if a[:2] == ["git", "push"]]
        self.assertTrue(pushes)
        for a in pushes:
            self.assertFalse([x for x in a if x.startswith("--force")
                              or x == "-f"], a)

    def test_new_branch_is_a_plain_push(self):
        git(self.wt, "checkout", "-b", "feat/new", "origin/main")
        commit_file(self.wt, "n.txt", "n\n", "new")
        self.assertEqual(self.forge.push(self.wt, "feat/new"),
                         forge_push.PUSHED)
        self.assertEqual(git(self.origin, "rev-parse", "feat/new"),
                         git(self.wt, "rev-parse", "HEAD"))


if __name__ == "__main__":
    unittest.main()

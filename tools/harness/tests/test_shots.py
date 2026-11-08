"""The daemon's screenshot job: UI branches get their pictures regenerated
detached before the PR opens, so no agent waits on a 10-minute build."""
import os
import subprocess
import tempfile
import unittest

import support
import test_pr_lifecycle as lc
from harness import shots
from harness.shots_io import RealIO
from test_pr_lifecycle import state


class FakeIO:
    def __init__(self, files=()):
        self.files = list(files)
        self.head_n = 0
        self.started = []
        self.running = True
        self.commits = 0
        self.has_pictures = True
        self.commit_error = None

    def head(self, wt):
        return f"h{self.head_n}"

    def changed(self, wt):
        return self.files

    def start(self, wt, log, rc):
        self.started.append((wt, log, rc))
        self.rc = rc
        return 4000 + len(self.started)

    def alive(self, pid):
        return self.running

    def finish(self, code):
        with open(self.rc, "w") as f:
            f.write(f"{code}\n")

    def commit(self, wt):
        if self.commit_error:
            raise RuntimeError(self.commit_error)
        if self.has_pictures:
            self.commits += 1
            self.head_n += 1
        return self.has_pictures


class Rule(unittest.TestCase):
    def test_ui_source_needs_pictures(self):
        self.assertTrue(shots.needs_pictures(["ui/qt/src/mixer.cpp"]))
        self.assertTrue(shots.needs_pictures(["tools/x.sh", "ui/qt/qml/A.qml"]))

    def test_tests_prose_and_other_areas_do_not(self):
        for files in ([], ["engine/a.cpp"], ["ui/qt/tests/ui_mixer.cpp"],
                      ["ui/README.md"], ["docs/images/a.png"]):
            self.assertFalse(shots.needs_pictures(files), files)


class Job(support.DaemonCase):
    # Only the helpers of Lifecycle: its own tests would rerun here with
    # a UI diff and wait on a job they never finish.
    feature = lc.Lifecycle.feature

    def make(self, files=("ui/qt/src/a.cpp",), **kw):
        kw.setdefault("floors", {"daw": 0})
        o = super().make(**kw)
        self.io = FakeIO(files)
        o.shots = shots.Shots(o, self.io)
        return o

    def opened(self):
        return len(self.forge.created)

    def test_no_ui_change_opens_the_pr_without_a_job(self):
        o = self.make(files=("engine/a.cpp",))
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: state(o, "f") == "pr_open"))
        self.assertEqual(self.io.started, [])

    def test_ui_change_waits_for_the_job_then_opens_the_pr(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        for _ in range(5):   # running: the PR does not open, nobody waits
            o.tick()
        self.assertEqual((self.opened(), len(self.io.started)), (0, 1))
        self.assertEqual(state(o, "f"), "integrating")
        self.io.finish(0)
        self.assertTrue(self.pump(o, lambda: state(o, "f") == "pr_open"))
        self.assertEqual((self.io.commits, len(self.io.started)), (1, 1))
        self.assertEqual(self.opened(), 1)
        kinds = [e["kind"] for e in o.store.events("f")]
        self.assertIn("shots_started", kinds)
        self.assertIn("shots_done", kinds)

    def test_pictures_are_committed_before_pre_pr(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        self.assertEqual(self.forge.pre_pr_calls, [])
        self.io.finish(0)
        self.assertTrue(self.pump(o, lambda: state(o, "f") == "pr_open"))
        self.assertEqual(self.io.commits, 1)
        self.assertEqual(len(self.forge.pre_pr_calls), 1)

    def test_job_that_fails_is_retried_once_then_blocks_with_the_log(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        with open(self.io.started[0][1], "w") as f:
            f.write("ninja: build stopped: subcommand failed\n")
        self.io.finish(2)
        o.tick()
        self.assertEqual(len(self.io.started), 1)   # backing off
        self.clock.offset += shots.RETRY_S + 1
        self.assertTrue(self.pump(o, lambda: len(self.io.started) == 2))
        self.io.finish(2)
        self.assertTrue(self.pump(o, lambda: state(o, "f") == "blocked"))
        self.assertEqual(self.forge.created, [])
        self.assertIn("subcommand failed", o.store.get_task("f")["note"])

    def test_job_that_dies_without_an_exit_code_fails(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        self.io.running = False
        o.tick()
        self.assertEqual(o.shots.info("f")["state"], "failed")
        self.assertIn("shots_failed",
                      [e["kind"] for e in o.store.events("f")])

    def test_commit_error_is_a_failed_run_not_a_crash(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        self.io.commit_error = "git commit failed: hook"
        self.io.finish(0)
        o.tick()
        self.assertEqual(o.shots.info("f")["state"], "failed")

    def test_unchanged_pictures_still_open_the_pr(self):
        o = self.make()
        self.io.has_pictures = False
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        self.io.finish(0)
        self.assertTrue(self.pump(o, lambda: state(o, "f") == "pr_open"))

    def test_a_new_head_after_done_runs_the_job_again(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        self.io.finish(0)
        o.tick()
        self.assertEqual(o.shots.info("f")["state"], "done")
        self.io.head_n += 5   # more commits after the pictures
        o.shots.step("f", "wt", "f", self.clock())
        self.assertEqual(len(self.io.started), 2)
        self.assertEqual(o.shots.info("f")["tries"], 1)

    def test_a_restart_keeps_polling_the_same_job(self):
        o = self.make()
        self.feature(o, "f")
        self.assertTrue(self.pump(o, lambda: self.io.started))
        o.shots = shots.Shots(o, self.io)   # state lives in the store
        o.tick()
        self.assertEqual(len(self.io.started), 1)
        self.io.finish(0)
        self.assertTrue(self.pump(o, lambda: state(o, "f") == "pr_open"))


class RealGit(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="shots-")
        self.addCleanup(lambda: __import__("shutil").rmtree(
            self.d, ignore_errors=True))
        self.git("init", "-q", "-b", "main")
        self.write("README", "x")
        self.git("add", "-A")
        self.commit("base")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")

    def git(self, *a):
        subprocess.run(["git", "-C", self.d, *a], check=True,
                       capture_output=True)

    def commit(self, msg):
        self.git("-c", "user.name=t", "-c", "user.email=t@t", "commit",
                 "-q", "-m", msg)

    def write(self, rel, text):
        p = os.path.join(self.d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)

    def test_changed_lists_the_branch_diff_against_main(self):
        self.write("ui/qt/src/a.cpp", "1")
        self.git("add", "-A")
        self.commit("ui")
        self.assertEqual(RealIO().changed(self.d), ["ui/qt/src/a.cpp"])

    def test_commit_takes_only_docs_images(self):
        self.write("docs/images/sonora-ui-edit.png", "png")
        self.write("stray.txt", "no")
        io = RealIO()
        self.assertTrue(io.commit(self.d))
        names = subprocess.run(
            ["git", "-C", self.d, "show", "--name-only", "--format="],
            capture_output=True, text=True).stdout.split()
        self.assertEqual(names, ["docs/images/sonora-ui-edit.png"])
        self.assertFalse(io.commit(self.d))   # nothing new: no empty commit


if __name__ == "__main__":
    unittest.main()

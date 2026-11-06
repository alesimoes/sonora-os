"""Per-step statuses of the local gate: parsing <step>.status, posting
local-ci/<step> once per change, the aggregate staying the only judge, and
local-ci.sh --list/--only/--resume against a throwaway repository (a fake
docker on PATH; no build runs)."""
import os
import shutil
import stat
import subprocess
import tempfile
import types
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import localci, localci_steps

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class Store:
    def __init__(self):
        self._d = {"meta": {}}
        self.events = []

    def get_meta(self, k):
        return self._d["meta"].get(k)

    def set_meta(self, k, v):
        self._d["meta"][k] = v

    def event(self, *a):
        self.events.append(a)


def write(d, step, state, seconds=5, sha="abc"):
    with open(os.path.join(d, step + ".status"), "w") as f:
        f.write("state=%s seconds=%d sha=%s\n" % (state, seconds, sha))


class Parse(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(localci_steps.parse_status("state=pass seconds=12 sha=ab\n"),
                         {"state": "pass", "seconds": 12, "sha": "ab"})
        self.assertIsNone(localci_steps.parse_status("state=weird seconds=1"))
        self.assertIsNone(localci_steps.parse_status("state=pass seconds=x"))
        self.assertIsNone(localci_steps.parse_status(""))


class Posting(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.d, True)
        self.posted = []
        self.store = Store()
        forge = types.SimpleNamespace(
            set_status=lambda sha, st, ctx, desc: self.posted.append((sha, st, ctx)))
        self.poster = localci_steps.StepPoster(self.store, forge)

    def test_pending_then_result_once_per_change(self):
        write(self.d, "tree", "running", 0)
        self.assertEqual(self.poster.sync("k", ["h1", "h2"], self.d), 1)
        self.assertEqual(self.poster.sync("k", ["h1", "h2"], self.d), 0)
        write(self.d, "tree", "pass", 90)
        write(self.d, "tsan", "fail", 7)
        self.assertEqual(self.poster.sync("k", ["h1", "h2"], self.d), 2)
        self.assertEqual(self.posted, [
            ("h1", "pending", "local-ci/tree"), ("h2", "pending", "local-ci/tree"),
            ("h1", "success", "local-ci/tree"), ("h2", "success", "local-ci/tree"),
            ("h1", "failure", "local-ci/tsan"), ("h2", "failure", "local-ci/tsan")])

    def test_a_restarted_poster_does_not_repost(self):
        write(self.d, "tree", "pass")
        self.poster.sync("k", ["h"], self.d)
        again = localci_steps.StepPoster(self.store, self.poster.forge)
        self.assertEqual(again.sync("k", ["h"], self.d), 0)

    def test_forge_error_does_not_raise(self):
        write(self.d, "tree", "pass")

        def boom(*a):
            raise RuntimeError("403")
        poster = localci_steps.StepPoster(self.store, types.SimpleNamespace(set_status=boom))
        poster.sync("k", ["h"], self.d)
        self.assertEqual(self.store.events[0][1], "step_status_failed")

    def test_clear_steps(self):
        write(self.d, "tree", "pass")
        localci_steps.clear_steps(self.d)
        self.assertEqual(localci_steps.read_steps(self.d), {})


class Aggregate(unittest.TestCase):
    def test_all_steps_green_is_not_enough_without_the_verdict(self):
        # per-step files never decide: only the script's own summary does
        self.assertFalse(localci.passed(0, "ok   tree (1 s)\nok   tsan (2 s)\n"))
        self.assertFalse(localci.passed(0, "ok   tree (1 s)\nFAIL tsan (2 s): l\n"
                                           "local-ci: failed: tsan\n"))
        self.assertTrue(localci.passed(0, "ok   tree (resumed, green for abc)\n"
                                          "local-ci: all passed\n"))

    def test_poll_posts_steps_before_the_aggregate(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        posted = []
        o = types.SimpleNamespace(
            root=tmp, store=Store(), cfg=types.SimpleNamespace(local_ci_slots=1),
            forge=types.SimpleNamespace(
                set_status=lambda sha, st, ctx, d: posted.append((st, ctx))))
        ci = localci.LocalCI(o, lambda: 1000.0)
        o.store.set_meta("localci:k", '{"state": "running", "dir": %s, '
                         '"heads": ["h"], "started": 1000}' % repr(tmp).replace("'", '"'))
        write(tmp, "tree", "pass")
        with open(os.path.join(tmp, "rc"), "w") as f:
            f.write("0")
        with open(os.path.join(tmp, "summary"), "w") as f:
            f.write("ok   tree\nlocal-ci: all passed\n")
        ci.poll()
        self.assertEqual(posted, [("success", "local-ci/tree"), ("success", "local-ci")])


def _bash():
    exe = localci.bash_exe()
    return exe if shutil.which(exe) or os.path.exists(exe) else None


@unittest.skipUnless(_bash(), "needs bash")
class ScriptFlags(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo = os.path.join(self.tmp, "repo").replace("\\", "/")
        os.makedirs(os.path.join(self.repo, "tools"))
        shutil.copy(os.path.join(REPO, "tools", "local-ci.sh"),
                    os.path.join(self.repo, "tools", "local-ci.sh"))
        for args in (["init", "-q"], ["add", "-A"],
                     ["-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "x"]):
            subprocess.run(["git"] + args, cwd=self.repo, check=True,
                           capture_output=True)
        self.sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.repo,
                                  capture_output=True, text=True).stdout.strip()
        # docker must never run from a unit test: a fake that always fails
        fake = os.path.join(self.tmp, "bin")
        os.makedirs(fake)
        path = os.path.join(fake, "docker")
        with open(path, "w", newline="\n") as f:
            f.write("#!/bin/sh\necho docker called >&2\nexit 1\n")
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
        self.out = os.path.join(self.tmp, "out").replace("\\", "/")
        self.env = dict(os.environ, SONORA_LOCAL_CI_ROOT=self.repo,
                        SONORA_LOCAL_CI_DIR=self.out,
                        PATH=fake.replace("\\", "/") + os.pathsep + os.environ["PATH"])

    def run_ci(self, *args):
        return subprocess.run(
            [localci.bash_exe(), self.repo + "/tools/local-ci.sh", "--full"] + list(args),
            env=self.env, capture_output=True, text=True, cwd=self.repo, timeout=120)

    def test_list_and_only(self):
        r = self.run_ci("--list")
        self.assertEqual(r.returncode, 0, r.stderr)
        steps = r.stdout.split()
        self.assertIn("tree", steps)
        self.assertIn("windows", steps)
        r = self.run_ci("--list", "--only", "tsan,tree")
        self.assertEqual(r.stdout.split(), ["tree", "tsan"])   # run order
        r = self.run_ci("--list", "--only", "nope")
        self.assertEqual(r.returncode, 2)
        self.assertIn("nope", r.stderr)

    def test_resume_skips_green_steps_for_this_head_only(self):
        steps = self.run_ci("--list").stdout.split()
        os.makedirs(self.out, exist_ok=True)
        for s in steps:
            write(self.out, s, "pass", 3, self.sha)
        r = self.run_ci("--resume")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("local-ci: all passed", r.stdout)
        self.assertEqual(r.stdout.count("(resumed"), len(steps))
        # another head's pass is not trusted: the step would run (and is
        # stopped here by a lock script that does not exist), so it FAILs
        write(self.out, "tree", "pass", 3, "0" * 40)
        r = self.run_ci("--resume", "--only", "tree")
        self.assertNotIn("resumed", r.stdout)
        self.assertIn("FAIL tree", r.stdout)
        with open(os.path.join(self.out, "tree.status")) as f:
            self.assertIn("state=fail", f.read())


if __name__ == "__main__":
    unittest.main()

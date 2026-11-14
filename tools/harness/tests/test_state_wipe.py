"""2026-11-14: a plan commit in the daemon checkout tracked state.json and
events.jsonl; the forced self-update dropped that commit and git deleted
both, so the daemon restarted on an empty store. Real git on throwaway
repositories, as in test_self_update."""
import json
import os
import shutil
import subprocess
import tempfile
import types
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import selfupdate, selfupdate_runtime
from test_self_update import Events, git, write

ODIR = "planning/orchestrator"
STATE = ODIR + "/state.json"
EVENTS = ODIR + "/events.jsonl"
DOC = json.dumps({"schema": 1, "features": {"f1": {"id": "f1"}},
                  "tasks": {}, "runs": {}, "reservations": [], "meta": {}})


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        return f.read()


class Store(Events):
    dirty = 0

    def mark_dirty(self):
        self.dirty += 1


class StateWipe(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        origin = os.path.join(self.tmp, "origin.git")
        self.dev = os.path.join(self.tmp, "dev")
        self.daemon = os.path.join(self.tmp, "daemon")
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main", origin],
                       check=True)
        subprocess.run(["git", "clone", "-q", origin, self.dev], check=True,
                       capture_output=True)
        shutil.copy(os.path.join(selfupdate.CODE_ROOT, ".gitignore"),
                    os.path.join(self.dev, ".gitignore"))
        write(self.dev, "tools/harness/train.py", "v1\n")
        git(self.dev, "checkout", "-q", "-b", "main")
        git(self.dev, "add", "-A")
        git(self.dev, "commit", "-qm", "base")
        git(self.dev, "push", "-q", "origin", "main")
        subprocess.run(["git", "clone", "-q", origin, self.daemon],
                       check=True, capture_output=True)
        self.store = Store()
        self.o = types.SimpleNamespace(
            store=self.store, cfg=types.SimpleNamespace(),
            broker=types.SimpleNamespace(stop=lambda: None))
        self.up = selfupdate.SelfUpdate(
            self.o, ["tools/orchestrator.py", "run", "--live"],
            root=self.daemon, restart=lambda argv: None)

    def push_harness(self):
        write(self.dev, "tools/harness/train.py", "v2\n")
        git(self.dev, "commit", "-qam", "harness change")
        git(self.dev, "push", "-q", "origin", "main")
        return git(self.dev, "rev-parse", "HEAD")

    def planner_commits_runtime(self, extra=()):
        """What the architect did: plan plus the runtime files, forced."""
        write(self.daemon, STATE, DOC)
        write(self.daemon, EVENTS, '{"kind": "a"}\n')
        write(self.daemon, ".harness/plan/x.plan.json", "{}")
        write(self.daemon, "planning/plans/x.md", "plan\n")
        for rel in extra:
            write(self.daemon, rel, "stray\n")
        git(self.daemon, "add", "-f", STATE, EVENTS, ".harness/plan",
            "planning/plans/x.md", *extra)
        git(self.daemon, "commit", "-qm", "x: plan reserved ADR 218")
        # the daemon keeps writing after the commit: tracked and modified
        write(self.daemon, STATE, DOC.replace("f1", "f2"))
        write(self.daemon, EVENTS, '{"kind": "a"}\n{"kind": "b"}\n')

    def assert_runtime_intact(self):
        self.assertEqual(read(self.daemon, STATE), DOC.replace("f1", "f2"))
        self.assertEqual(read(self.daemon, EVENTS),
                         '{"kind": "a"}\n{"kind": "b"}\n')
        tracked = git(self.daemon, "ls-files", ODIR, ".harness")
        self.assertEqual(tracked, "")

    def test_forced_update_keeps_state_and_events(self):
        # the incident: a foreign file in the commit, forced at the bound
        self.planner_commits_runtime(extra=("engine/stray.cpp",))
        new = self.push_harness()
        self.assertFalse(self.up.ready(1000))
        self.assertTrue(self.up.ready(1000 + self.up.max_defer_s))
        # the mixed commit is not replayed (it waits on its keep/ branch)
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)
        self.assert_runtime_intact()
        self.assertTrue(os.path.exists(
            os.path.join(self.daemon, ".harness/plan/x.plan.json")))
        self.assertIn("self_update_runtime_restored", self.store.kinds())
        self.assertGreaterEqual(self.store.dirty, 1)

    def test_runtime_in_a_plan_commit_is_stripped_without_waiting(self):
        # modified runtime files neither block the update nor stay tracked
        self.planner_commits_runtime()
        new = self.push_harness()
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD~1"), new)
        self.assertEqual(git(self.daemon, "show", "--name-only",
                             "--format=", "HEAD"), "planning/plans/x.md")
        self.assert_runtime_intact()

    def test_the_runtime_copy_aside_is_on_disk_and_pruned(self):
        write(self.daemon, STATE, DOC)
        for n in range(selfupdate_runtime.KEEP_ASIDE + 2):
            rt = selfupdate_runtime.Runtime(self.daemon, self.up.git,
                                            1000 + n)
        self.assertEqual(read(rt.aside, STATE), DOC)
        base = os.path.join(self.daemon, selfupdate_runtime.ASIDE)
        self.assertEqual(len(os.listdir(base)),
                         selfupdate_runtime.KEEP_ASIDE)

    def test_events_that_only_grew_are_not_rolled_back(self):
        write(self.daemon, EVENTS, "a\n")
        rt = selfupdate_runtime.Runtime(self.daemon, self.up.git, 1000)
        write(self.daemon, EVENTS, "a\nb\n")
        self.assertEqual(rt.restore(), [])
        self.assertEqual(read(self.daemon, EVENTS), "a\nb\n")
        write(self.daemon, EVENTS, "other\n")   # git replaced it
        self.assertEqual(rt.restore(), [EVENTS])
        self.assertEqual(read(self.daemon, EVENTS), "a\n")


if __name__ == "__main__":
    unittest.main()

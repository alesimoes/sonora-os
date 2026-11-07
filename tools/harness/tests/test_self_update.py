"""The live daemon re-execs itself when origin/main changes its harness
(2026-11-07: it ran a day-old harness). Real git on throwaway repositories;
the re-exec itself is a stub that records the arguments."""
import os
import shutil
import subprocess
import tempfile
import types
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import selfupdate


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t"]
                          + list(args), cwd=cwd, check=True,
                          capture_output=True, text=True).stdout.strip()


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", newline="\n") as f:
        f.write(text)


class Events:
    def __init__(self):
        self.rows, self.flushed = [], 0

    def event(self, *a):
        self.rows.append(a)

    def flush(self):
        self.flushed += 1

    def kinds(self):
        return [r[1] for r in self.rows]


class SelfUpdate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.origin = os.path.join(self.tmp, "origin.git")
        self.dev = os.path.join(self.tmp, "dev")      # pushes to main
        self.daemon = os.path.join(self.tmp, "daemon")
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main",
                        self.origin], check=True)
        subprocess.run(["git", "clone", "-q", self.origin, self.dev],
                       check=True, capture_output=True)
        write(self.dev, "tools/harness/train.py", "v1\n")
        write(self.dev, "engine/x.cpp", "v1\n")
        git(self.dev, "checkout", "-q", "-b", "main")
        git(self.dev, "add", "-A")
        git(self.dev, "commit", "-qm", "base")
        git(self.dev, "push", "-q", "origin", "main")
        subprocess.run(["git", "clone", "-q", self.origin, self.daemon],
                       check=True, capture_output=True)
        self.store = Events()
        self.running = []
        self.restarted = []
        self.released = []
        self.o = types.SimpleNamespace(
            store=self.store, cfg=types.SimpleNamespace(),
            broker=types.SimpleNamespace(stop=lambda: None))
        self.up = selfupdate.SelfUpdate(
            self.o, ["tools/orchestrator.py", "run", "--live"],
            root=self.daemon, restart=self.restarted.append,
            release=lambda: self.released.append(1))

    def push(self, rel, text):
        write(self.dev, rel, text)
        git(self.dev, "add", "-A")
        git(self.dev, "commit", "-qm", "change " + rel)
        git(self.dev, "push", "-q", "origin", "main")
        return git(self.dev, "rev-parse", "HEAD")

    def test_a_product_change_does_not_restart(self):
        self.push("engine/x.cpp", "v2\n")
        self.assertFalse(self.up.ready(1000))

    def test_a_harness_change_moves_the_checkout_and_restarts(self):
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)
        self.up.restart()
        self.assertEqual(self.store.flushed, 1)
        self.assertEqual(self.restarted[0][1:], ["run", "--live"])
        self.assertTrue(os.path.isabs(self.restarted[0][0]))
        self.assertIn("self_update", self.store.kinds())

    def test_modified_tracked_files_block_the_update(self):
        self.push("tools/harness/train.py", "v2\n")
        write(self.daemon, "engine/x.cpp", "local edit\n")
        self.assertFalse(self.up.ready(1000))
        self.assertIn("self_update_blocked", self.store.kinds())
        self.assertEqual(self.restarted, [])

    def test_checks_at_most_every_check_s(self):
        self.assertFalse(self.up.ready(1000))
        self.push("tools/build-slot.sh", "v2\n")
        self.assertFalse(self.up.ready(1000 + 10))
        self.assertTrue(self.up.ready(1000 + selfupdate.CHECK_S))


class DaemonLoop(support.DaemonCase):
    def test_run_loop_restarts_when_the_updater_is_ready(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        calls = []
        o.updater = types.SimpleNamespace(ready=lambda now: True,
                                          restart=lambda: calls.append(1))
        o.run(max_ticks=5, sleep=lambda s: None)
        self.assertEqual(calls, [1])


if __name__ == "__main__":
    unittest.main()

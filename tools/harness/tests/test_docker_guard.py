"""harness/docker_guard: while Docker is down the pools that build or test
are paused, probed at most once a minute, with one event per change
(owner 2026-11-09: no relaunch loop when Docker cannot start)."""
import os
import unittest

import support
from harness import docker_guard as D
from harness.store import Store


class Guard(unittest.TestCase):
    def setUp(self):
        self.tmp = support.tempfile.mkdtemp(prefix="dg-")
        self.addCleanup(support.shutil.rmtree, self.tmp, True)
        self.store = Store(os.path.join(self.tmp, "state.json"))
        self.addCleanup(self.store.close)
        self.answers, self.calls = [], 0

    def probe(self):
        self.calls += 1
        return self.answers.pop(0)

    def kinds(self):
        return [e["kind"] for e in self.store.events("daemon", limit=20)]

    def test_down_pauses_the_docker_pools_until_it_answers_again(self):
        g = D.Guard(self.store, self.probe)
        self.answers = [True, False, True]
        self.assertEqual(g.held(0), frozenset())
        self.assertEqual(g.held(30), frozenset())          # cached
        self.assertEqual(g.held(60), D.DOCKER_POOLS)
        self.assertEqual(g.held(100), D.DOCKER_POOLS)      # still cached
        self.assertEqual(g.held(120), frozenset())
        self.assertEqual(self.calls, 3)
        self.assertEqual(sorted(self.kinds()), ["docker_down", "docker_up"])

    def test_the_planner_pool_is_never_held(self):
        self.assertNotIn("plan", D.DOCKER_POOLS)
        self.assertNotIn("principal", D.DOCKER_POOLS)


if __name__ == "__main__":
    unittest.main()

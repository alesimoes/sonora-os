"""harness/docker_guard: while Docker is down the pools that build or test
are paused, probed at most once a minute, with one event per change
(owner 2026-11-09: no relaunch loop when Docker cannot start)."""
import json
import os
import unittest

import support
from harness import docker_guard as D, scheduler
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

    def test_status_names_the_outage_apart_from_owner_pauses(self):
        # 2026-11-13: status.json showed the held pools as `paused`, the
        # same words as an owner pause; nothing said Docker was down.
        g = D.Guard(self.store, self.probe)
        self.answers = [False, False, True]
        g.held(100)
        g.held(160)
        pools = {"build": {}, "build:daw": {}, "plan": {}}
        st = g.status(pools)
        self.assertEqual(st["docker_down_since"], 100)
        self.assertEqual(st["docker_held"], sorted(D.DOCKER_POOLS))
        self.assertTrue(pools["build"]["docker_down"])
        self.assertTrue(pools["build:daw"]["docker_down"])
        self.assertFalse(pools["plan"]["docker_down"])
        g.held(220)
        self.assertEqual(g.status(), {"docker_down_since": None,
                                      "docker_held": []})

    def test_the_planner_pool_is_never_held(self):
        self.assertNotIn("plan", D.DOCKER_POOLS)
        self.assertNotIn("principal", D.DOCKER_POOLS)


class SchedulerGap(unittest.TestCase):
    """The gap of a held track reads "docker down", not "paused"."""

    def setUp(self):
        self.tmp = support.tempfile.mkdtemp(prefix="dg-")
        self.addCleanup(support.shutil.rmtree, self.tmp, True)
        self.store = Store(os.path.join(self.tmp, "state.json"))
        self.addCleanup(self.store.close)
        for i in range(2):
            self.store.add_task(f"t{i}", "daw", "rt-engineer",
                                files=[f"daw/t{i}/**"])

    def plan(self, **kw):
        return scheduler.plan(self.store.tasks(), [], scheduler.Config(),
                              1000.0, **kw)

    def test_docker_held_launches_no_engineer_and_names_docker(self):
        p = self.plan(docker_held=D.DOCKER_POOLS)
        self.assertEqual(p.launches, [])
        self.assertEqual(p.gaps["daw"], "docker down")

    def test_an_owner_pause_still_reads_paused(self):
        p = self.plan(paused={"build"})
        self.assertEqual(p.gaps["daw"], "paused")
        self.assertNotEqual(self.plan().gaps.get("daw"), "docker down")


class DaemonStatus(support.DaemonCase):
    """One daemon tick with Docker down: nothing launches, status.json
    names Docker, and the owner's paused.json stays what the owner wrote."""

    def test_status_json_says_docker_down_not_paused(self):
        o = self.make(floors={"daw": 1})
        o.store.add_task("a", "daw", "rt-engineer", files=["daw/a/**"])
        o.docker = D.Guard(o.store, lambda: False)
        o.tick()
        self.assertEqual(o.store.active_runs(), [])
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        self.assertEqual(st["paused"], [])
        self.assertIsNotNone(st["docker_down_since"])
        self.assertEqual(st["docker_held"], sorted(D.DOCKER_POOLS))
        self.assertTrue(st["pools"]["build:daw"]["docker_down"])
        self.assertEqual(st["pools"]["build:daw"]["reason"], "docker down")


if __name__ == "__main__":
    unittest.main()

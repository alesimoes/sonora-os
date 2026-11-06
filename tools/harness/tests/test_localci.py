"""harness/localci.py: batching, bisecting and the heavy-lock priority, with
git and bash replaced (no build runs here)."""
import os
import tempfile
import types
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
from harness import localci


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


class Clock:
    t = 1000.0

    def __call__(self):
        return self.t


class Batching(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, self.tmp, True)
        self.posted, self.popen, self.conflicts = [], [], set()
        forge = types.SimpleNamespace(
            set_status=lambda sha, st, ctx, d: self.posted.append((sha, st)))
        o = types.SimpleNamespace(
            root=self.tmp, store=Store(), forge=forge,
            cfg=types.SimpleNamespace(local_ci_slots=1))
        self.clock = Clock()
        self.ci = localci.LocalCI(o, self.clock)

        def checkout(wt, heads):
            ok = [h for h in heads if h not in self.conflicts]
            return None, [(h, "conflicts with main") for h in heads
                          if h in self.conflicts], ok
        self.ci._checkout = checkout
        p = mock.patch.object(localci.subprocess, "Popen",
                              side_effect=lambda *a, **k: self.popen.append(k["env"]))
        p.start()
        self.addCleanup(p.stop)

    def finish(self, rc, summary=None):
        if summary is None:
            summary = ("ok   tree\nlocal-ci: all passed\n" if rc == 0
                       else "FAIL tree\nlocal-ci: failed: tree\n")
        (key, r), = self.ci._running()
        with open(os.path.join(r["dir"], "rc"), "w") as f:
            f.write(str(rc))
        with open(os.path.join(r["dir"], "summary"), "w") as f:
            f.write(summary)
        self.ci.poll()

    def ready(self, *heads):
        for h in heads:
            self.ci.start(h)
        self.clock.t += localci.BATCH_SETTLE_S
        self.ci.poll()

    def state(self, h):
        return self.ci.result(h)["state"]

    def test_ready_prs_share_one_run_and_one_verdict(self):
        self.ready("a", "b", "c")
        self.assertEqual(len(self.popen), 1)
        self.assertEqual(self.popen[0]["SONORA_HEAVY_PRIORITY"], "0")
        self.assertEqual([self.state(h) for h in "abc"], ["running"] * 3)
        self.finish(0, "ok tree\nlocal-ci: all passed\n")
        self.assertEqual([self.state(h) for h in "abc"], ["pass"] * 3)
        self.assertIn(("b", "success"), self.posted)

    def test_wait_for_the_settle_time(self):
        self.ci.start("a")
        self.ci.poll()
        self.assertEqual(self.popen, [])

    def test_red_batch_is_bisected_down_to_the_culprit(self):
        self.ready("a", "b", "c", "d")
        self.finish(1, "FAIL tree\n")
        self.assertEqual(self.state("a"), "running")   # not judged yet
        # halves [a, b] and [c, d]; one checkout, so one at a time
        self.assertEqual(len(self.popen), 2)
        self.finish(0)                      # a+b green
        self.assertEqual((self.state("a"), self.state("b")), ("pass", "pass"))
        self.finish(1, "FAIL tree\n")       # c+d red -> [c], [d]
        self.finish(0)                      # c green
        self.assertEqual(self.state("c"), "pass")
        self.finish(1, "FAIL tree\n")       # d alone is red: the culprit
        self.assertEqual(self.state("d"), "fail")
        self.assertEqual(self.ci.result("d")["queued"], False)
        self.assertEqual(self.ci._queue("groups"), [])

    def test_a_conflicting_head_is_dropped_not_the_batch(self):
        self.conflicts = {"b"}
        self.ready("a", "b")
        self.assertEqual(self.state("b"), "fail")
        self.assertIn("conflicts", self.ci.result("b")["summary"])
        self.finish(0)
        self.assertEqual(self.state("a"), "pass")

    def test_pre_pr_runs_go_after_the_batch_with_priority_two(self):
        self.ci.start("feat", pre=True)
        self.ci.start("a")
        self.clock.t += localci.BATCH_SETTLE_S
        self.ci.poll()
        self.assertEqual(self.popen[0]["SONORA_HEAVY_PRIORITY"], "0")
        self.finish(0)
        self.assertEqual(self.popen[1]["SONORA_HEAVY_PRIORITY"], "2")
        self.finish(0)
        self.assertEqual(self.state("feat"), "pass")

    def test_main_full_run_has_priority_one(self):
        self.assertTrue(self.ci.start("m" * 40, full=True))
        self.assertEqual(self.popen[0]["SONORA_HEAVY_PRIORITY"], "1")
        self.assertFalse(self.ci.start("n" * 40, full=True))  # no checkout

    def test_builder_status_names_batch_queue_and_last(self):
        self.ready("a", "b")
        self.ci.start("c")
        b = self.ci.builder_status()
        self.assertEqual(b["running"]["heads"], ["a", "b"])
        self.assertEqual(b["queue"]["batch"], ["c"])
        self.finish(0, "local-ci: all passed\n")
        self.assertEqual(self.ci.builder_status()["last"]["state"], "pass")


if __name__ == "__main__":
    unittest.main()

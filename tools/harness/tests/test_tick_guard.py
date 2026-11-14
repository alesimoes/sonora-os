"""harness/tick_guard: one bad item never stops the tick (2026-11-08: a
tester finding with the wrong shape killed every tick for an hour, #504)."""
import json
import os
import unittest

import support
from harness import tick_guard
from harness.store import Store
import test_pr_lifecycle as lc
from test_pr_lifecycle import state

GREEN = {"Gate": "pass"}


class Unit(unittest.TestCase):
    def setUp(self):
        self.tmp = support.tempfile.mkdtemp(prefix="guard-")
        self.store = Store(os.path.join(self.tmp, "state.json"))
        self.logged = []
        self.g = tick_guard.Guard(self.store, lambda: 100.0,
                                  self.logged.append)

    def tearDown(self):
        self.store.close()
        support.shutil.rmtree(self.tmp, ignore_errors=True)

    def boom(self, x=None):
        raise ValueError(f"bad {x}")

    def test_step_times_add_up_per_tick_and_reset(self):
        clock = iter([0.0, 2.0, 2.0, 3.0, 3.0, 3.5, 10.0, 11.0])
        self.g._timer = lambda: next(clock)
        self.g.step("watch", lambda: None)
        self.g.each("poll", [1, 2], lambda x: None)
        self.assertEqual(self.g.slowest(), [["watch", 2.0], ["poll", 1.5]])
        self.g.begin_tick()
        self.g.step("watch", self.boom)    # a failed step is timed too
        self.assertEqual(self.g.slowest(), [["watch", 1.0]])

    def test_error_is_one_event_then_counted_and_shown(self):
        for _ in range(3):
            self.assertIsNone(self.g.step("poll", self.boom, "a", key="a",
                                          task="t"))
        evs = self.store.events("t", "step_error")
        self.assertEqual(len(evs), 1)
        self.assertIn("poll[a]: ValueError: bad a", evs[0]["detail"])
        self.assertEqual(len(self.logged), 1)   # traceback once
        rows = self.g.rows()
        self.assertEqual((rows[0]["id"], rows[0]["count"], rows[0]["check"]),
                         ("tick:poll:a", 3, "tick"))

    def test_success_clears_the_alert_and_a_new_text_is_a_new_event(self):
        self.g.step("s", self.boom, 1, key=1)
        self.g.step("s", self.boom, 2, key=1)   # text changed: logged again
        self.assertEqual(len(self.store.events(None, "step_error")), 2)
        self.assertEqual(self.g.step("s", lambda: "ok", key=1), "ok")
        self.assertEqual(self.g.rows(), [])

    def test_each_isolates_items_and_stops_on_false(self):
        seen = []

        def fn(x, y):
            seen.append((x, y))
            if x == 2:
                raise RuntimeError("two")
            return x != 3

        self.assertFalse(self.g.each("e", [1, 2, 3, 4], fn, "y"))
        self.assertEqual(seen, [(1, "y"), (2, "y"), (3, "y")])
        self.assertEqual([r["id"] for r in self.g.rows()], ["tick:e:2"])


class Daemon(support.DaemonCase):
    """Not a Lifecycle subclass: that would re-run its whole suite here."""
    feature, open_feature = lc.Lifecycle.feature, lc.Lifecycle.open_feature

    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def test_one_pr_poll_crash_does_not_stop_the_next_merge_or_status(self):
        o = self.make()
        self.open_feature(o, "bad", 100, checks=GREEN)
        self.open_feature(o, "good", 101, checks=GREEN)
        real = self.forge.pr_status

        def pr_status(n):
            if n == 100:
                raise KeyError("not a ForgeError")   # the unforeseen kind
            return real(n)
        self.forge.pr_status = pr_status
        self.assertTrue(self.pump(o, lambda: state(o, "good") == "merged"))
        self.assertEqual(state(o, "bad"), "pr_open")
        self.assertEqual(len(o.store.events("bad", "step_error")), 1)
        with open(o._p("status.json"), encoding="utf-8") as f:
            alerts = json.load(f)["alerts"]
        self.assertEqual([a["id"] for a in alerts], ["tick:watch_pr:bad"])
        self.assertIn("KeyError", alerts[0]["text"])

    def test_one_bad_result_does_not_stop_the_other_runs(self):
        o = self.make(floors={"daw": 2})
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        o.store.add_task("b", "daw", "rt-engineer", files=["b/**"])
        real = o._on_result

        def on_result(r, data, now):
            if r["task_id"] == "a":
                raise AttributeError("'str' object has no attribute 'get'")
            return real(r, data, now)
        o._on_result = on_result
        self.assertTrue(self.pump(o, lambda: state(o, "b") == "qa"))
        self.assertEqual(len(o.store.events("a", "step_error")), 1)
        self.assertEqual(o.store.events(None, "tick_error"), [])
        self.assertTrue(os.path.exists(o._p("status.json")))


if __name__ == "__main__":
    unittest.main()

"""pool_under_floor lead wakes: none while a feature is coming, one per
cooldown otherwise (cost bug 2026-11-08)."""
import unittest

import support
from harness import leadwake


def ft(track, state):
    return {"track": track, "state": state}


def wk(n, created, track="daw"):
    return {"id": f"wake-lead-pool_under_floor-{track}-{n}",
            "created": created}


W = [("lead", "pool_under_floor", "daw")]


class Pure(unittest.TestCase):
    def test_busy_feature_suppresses_only_its_track(self):
        for st in ("queued", "planning", "distributing"):
            self.assertEqual(leadwake.allowed(W, [ft("daw", st)], [], 0, 1800), [])
        self.assertEqual(leadwake.allowed(W, [ft("os", "planning")], [], 0, 1800), W)
        self.assertEqual(leadwake.allowed(W, [ft("daw", "building")], [], 0, 1800), W)

    def test_cooldown(self):
        wakes = [wk(1, 1000)]
        self.assertEqual(leadwake.allowed(W, [], wakes, 2000, 1800), [])
        self.assertEqual(leadwake.allowed(W, [], wakes, 2801, 1800), W)

    def test_other_wakes_pass(self):
        other = [("architect", "bug_ruling", "x")]
        self.assertEqual(leadwake.allowed(other, [ft("daw", "planning")],
                                          [wk(1, 5)], 6, 1800), other)

    def test_count(self):
        self.assertEqual(leadwake.count_last_hour(
            [wk(1, 100), wk(2, 5000)], 5100), 1)


class Daemon(support.DaemonCase):
    def ticks(self, o, n, step=30):
        for _ in range(n):
            for w in o.store.tasks(kind="wake"):
                o.store.set_task(w["id"], state="merged")  # served
            o.tick()
            self.clock.offset += step

    def wakes(self, o):
        return [w for w in o.store.tasks(kind="wake")
                if "pool_under_floor" in w["id"]]

    def test_feature_planning_means_no_lead_wake(self):
        o = self.make(dry_run=True, floors={"daw": 1})
        o.store.ensure_feature("f1", "daw")
        o.store.set_feature("f1", state="planning")
        self.ticks(o, 10)
        self.assertEqual(self.wakes(o), [])

    def test_without_feature_one_wake_per_cooldown(self):
        o = self.make(dry_run=True, floors={"daw": 1})
        self.ticks(o, 10)
        self.assertEqual(len(self.wakes(o)), 1)
        self.clock.offset += o.cfg.lead_wake_cooldown_s
        self.ticks(o, 10)
        self.assertEqual(len(self.wakes(o)), 2)


if __name__ == "__main__":
    unittest.main()

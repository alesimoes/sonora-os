"""A blocked wake must not suppress a new wake for the same event."""
import unittest

import support
from harness import wakes


class Pending(unittest.TestCase):
    def test_only_queued_suppresses(self):
        mk = lambda st: [{"id": "wake-lead-x-1", "state": st}]
        self.assertTrue(wakes.is_pending(mk("queued"), "wake-lead-x"))
        self.assertFalse(wakes.is_pending(mk("blocked"), "wake-lead-x"))
        self.assertFalse(wakes.is_pending(mk("queued"), "wake-lead-y"))


class Daemon(support.DaemonCase):
    def ids(self, o):
        return [w["id"] for w in o.store.tasks(kind="wake")]

    def test_blocked_wake_does_not_suppress_next(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o._ensure_wake("lead", "pool_under_floor", "daw")
        o._ensure_wake("lead", "pool_under_floor", "daw")
        self.assertEqual(len(self.ids(o)), 1)
        o.store.set_task(self.ids(o)[0], state="blocked")
        o._ensure_wake("lead", "pool_under_floor", "daw")
        self.assertEqual(len(self.ids(o)), 2)


if __name__ == "__main__":
    unittest.main()

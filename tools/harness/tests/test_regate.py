"""A fixer's pushed head is queued for the local gate at once, and the heads
it replaced leave the local-CI queues (#446, 2026-11-06)."""
import support  # noqa: F401  (sets sys.path)
from test_merge_train import LocalGate


class Regate(LocalGate):
    def adopted(self):
        o = self.make()
        old = self.green(o)
        o.tick()
        return o, old

    def test_fixer_head_goes_to_the_local_gate(self):
        o, old = self.adopted()
        o.train.after_fix("pr-10", "newhead1")
        self.assertIn("newhead1", o.localci.started)
        self.assertEqual(o.train.info("pr-10")["old_heads"], [old])

    def test_fixer_that_changed_nothing_does_not_requeue(self):
        o, old = self.adopted()
        before = list(o.localci.started)
        o.train.after_fix("pr-10", old)
        self.assertEqual(o.localci.started, before)

    def test_replaced_head_is_gone_and_current_one_is_not(self):
        o, old = self.adopted()
        o.train.after_fix("pr-10", "newhead1")
        o.train._save("pr-10", head="newhead1")
        self.assertTrue(o.train.head_is_gone(old))
        self.assertFalse(o.train.head_is_gone("newhead1"))

    def test_refresh_seen_in_a_step_remembers_the_old_head(self):
        o, old = self.adopted()
        self.forge.prs[10]["head"] = "pushed2"
        self.clock.offset += 200
        o.tick()
        i = o.train.info("pr-10")
        self.assertEqual((i["head"], i["old_heads"]), ("pushed2", [old]))
        self.assertTrue(o.train.head_is_gone(old))


if __name__ == "__main__":
    import unittest
    unittest.main()

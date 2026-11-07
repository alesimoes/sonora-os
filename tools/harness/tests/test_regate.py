"""A fixer's pushed head replaces the one it fixed: the old head is
remembered and never judged as the new one (#446, 2026-11-06)."""
import support  # noqa: F401  (sets sys.path)
from test_merge_train import Train


class Regate(Train):
    def adopted(self):
        o = self.make()
        self.forge.add_pr(10, "a", ["engine/x.cpp"],
                          checks={"lint": "pass", "Micro build": "pending"})
        o.train.adopt()
        old = self.forge.pr_detail(10)["head_sha"]
        o.tick()
        return o, old

    def test_fixer_push_remembers_the_old_head(self):
        o, old = self.adopted()
        o.train.after_fix("pr-10", "newhead1")
        self.assertEqual(o.train.info("pr-10")["old_heads"], [old])
        self.assertEqual(o.train.info("pr-10")["phase"], "CI rodando")

    def test_fixer_that_changed_nothing_reruns_instead(self):
        o, old = self.adopted()
        o.train.after_fix("pr-10", old)
        self.assertEqual(o.train.info("pr-10")["phase"], "rerodando")

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

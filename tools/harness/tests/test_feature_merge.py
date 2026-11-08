"""The daemon merges its own green feature PRs (2026-11-08: #507/#509/#510
sat green and CLEAN because only a one-shot script owned the merge)."""
import unittest

import support
from harness import feature_merge
import test_pr_lifecycle as lc
from test_pr_lifecycle import state

GREEN = {"Gate": "pass", "micro": "pass"}


def detail(**kw):
    d = {"state": "open", "base": "main", "mergeable": "yes",
         "checks": [{"name": "Gate", "state": "pass"}]}
    d.update(kw)
    return d


class Rule(unittest.TestCase):
    def test_green_gate_clean_is_ready(self):
        self.assertTrue(feature_merge.ready_to_merge(detail())[0])

    def test_refusals(self):
        bad = [detail(state="closed"), detail(base="feat/x"),
               detail(mergeable="unknown"), detail(mergeable="no"),
               detail(checks=[]),
               detail(checks=[{"name": "micro", "state": "pass"}]),
               detail(checks=[{"name": "Gate", "state": "pass"},
                              {"name": "x", "state": "pending"}]),
               detail(checks=[{"name": "Gate", "state": "cancel"}])]
        for d in bad:
            self.assertFalse(feature_merge.ready_to_merge(d)[0], d)


class WatchMerges(lc.Lifecycle):
    def test_green_feature_pr_merged_in_one_watch_cycle(self):
        o = self.make()
        self.open_feature(o, "g", 100, checks=GREEN)
        self.forge.prs[100]["head"] = "abc12345"
        self.assertTrue(self.pump(o, lambda: state(o, "g") == "merged"))
        self.assertEqual(self.forge.merged, [(100, "abc12345")])
        self.assertEqual(self.forge.enqueued, [])  # no detached script

    def test_pending_or_conflicting_is_not_merged(self):
        o = self.make()
        self.open_feature(o, "p", 100, checks={"Gate": "pending"})
        self.open_feature(o, "c", 101, checks=GREEN, mergeable="no")
        self.pump(o, lambda: False, timeout=0.3)
        self.assertFalse(getattr(self.forge, "merged", []))

    def test_forge_refusal_is_an_event_not_a_crash(self):
        o = self.make()
        self.open_feature(o, "g", 100, checks=GREEN)
        self.forge.fail.add("merge")
        self.assertTrue(self.pump(o, lambda: o.store.events(
            kind="feature_merge_failed")))
        self.assertEqual(state(o, "g"), "pr_open")


if __name__ == "__main__":
    unittest.main()

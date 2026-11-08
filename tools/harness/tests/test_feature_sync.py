"""Feature state follows its PR and its tasks (pr-442 stuck in pr_open for
68 h; mixassist.plans stuck in blocked after its task closed)."""
import unittest

import support
from harness import feature_sync


class FeatureSync(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def feature(self, o, fid, task_state, fstate, pr=None):
        o.store.add_task(fid, "daw", "rt-engineer", files=[f"{fid}/**"],
                         feature_id=fid, state=task_state)
        o.store.set_feature(fid, state=fstate, pr=pr)

    def sync(self, o):
        self.clock.offset += 200   # past ci_poll_s
        feature_sync.sync(o, o.clock())

    def fstate(self, o, fid):
        return o.store.get_feature(fid)["state"]

    def test_merged_pr_makes_feature_merged(self):
        o = self.make()
        self.feature(o, "pr-442", "ci_fix", "pr_open", pr=442)
        self.forge.prs[442] = {"branch": "b", "files": [], "state": "merged",
                               "mergeable": "yes", "checks": {}}
        self.sync(o)
        self.assertEqual(self.fstate(o, "pr-442"), "merged")
        self.assertEqual(o.store.get_task("pr-442")["state"], "merged")

    def test_closed_pr_makes_feature_closed(self):
        o = self.make()
        self.feature(o, "c", "ci_fix", "pr_open", pr=7)
        self.forge.prs[7] = {"branch": "b", "files": [], "state": "closed",
                             "mergeable": "yes", "checks": {}}
        self.sync(o)
        self.assertEqual(self.fstate(o, "c"), "closed")
        self.assertEqual(o.store.get_task("c")["state"], "closed")

    def test_blocked_feature_with_all_tasks_closed_leaves_blocked(self):
        o = self.make()
        self.feature(o, "mixassist.plans", "closed", "blocked")
        self.sync(o)
        self.assertEqual(self.fstate(o, "mixassist.plans"), "closed")

    def test_blocked_feature_with_a_merged_task_becomes_merged(self):
        o = self.make()
        self.feature(o, "m", "merged", "blocked")
        self.sync(o)
        self.assertEqual(self.fstate(o, "m"), "merged")

    def test_open_pr_and_live_blocked_feature_are_left_alone(self):
        o = self.make()
        self.feature(o, "open", "ci_fix", "pr_open", pr=9)
        self.forge.prs[9] = {"branch": "b", "files": [], "state": "open",
                             "mergeable": "yes", "checks": {}}
        self.feature(o, "live", "blocked", "blocked")
        self.sync(o)
        self.assertEqual(self.fstate(o, "open"), "pr_open")
        self.assertEqual(self.fstate(o, "live"), "blocked")


if __name__ == "__main__":
    unittest.main()

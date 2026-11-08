"""The automatic sweep adopts a PR no task owns even when its branch is a
sub-branch or belongs to a finished task (#519, 2026-11-08), and still leaves
alone the branch of a feature that is about to open its own PR."""
import support


class AutoAdoptOrphans(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def events(self, o):
        return [e["detail"] for e in o.store.events(kind="adopted_pr")]

    def test_pr_from_a_sub_branch_of_a_live_task_is_adopted(self):
        o = self.make()
        o.store.add_task("t1", "daw", "rt-engineer", state="building",
                         branch="sub/t1/dialog")
        self.forge.add_pr(519, "sub/t1/dialog", ["engine/x.cpp"])
        self.assertEqual(o.train.adopt(auto=True), [519])
        self.assertEqual(self.events(o), ["#519 sub/t1/dialog auto"])

    def test_pr_from_the_branch_of_a_blocked_task_is_adopted(self):
        o = self.make()
        o.store.add_task("t2", "daw", "rt-engineer", state="blocked",
                         branch="feat/t2")
        self.forge.add_pr(520, "feat/t2", ["engine/x.cpp"])
        self.assertEqual(o.train.adopt(auto=True), [520])

    def test_branch_of_a_feature_still_building_is_left_to_it(self):
        o = self.make()
        o.store.add_task("t3", "daw", "rt-engineer", state="building",
                         branch="feat/t3")
        self.forge.add_pr(521, "feat/t3", ["engine/x.cpp"])
        self.assertEqual(o.train.adopt(auto=True), [])

    def test_the_tick_adopts_the_orphan_and_logs_the_event(self):
        o = self.make()
        o.store.add_task("t1", "daw", "rt-engineer", state="building",
                         branch="sub/t1/x")
        self.forge.add_pr(519, "sub/t1/x", ["engine/x.cpp"])
        o.tick()
        self.assertTrue(o.train.is_adopted("pr-519"))
        self.assertIn("#519 sub/t1/x auto", self.events(o))

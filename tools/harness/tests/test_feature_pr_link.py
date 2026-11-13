"""A PR opened by hand on the branch of a feature stuck before its own PR is
recorded on that feature (ui.editing.commands and #651, 2026-11-09: two
slices integrating, one blocked, the PR green and owned by nobody for 9 h)."""
import support

BR = "feat/f1"


class FeaturePrLink(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def feature(self, o, tests_state="blocked"):
        o.store.add_feature("f1", "daw", branch=BR)
        for name, st in (("contract", "integrating"), ("ui", "integrating"),
                         ("tests", tests_state)):
            o.store.add_task(f"f1.{name}", "daw", "ui-engineer", state=st,
                             feature_id="f1", branch=f"sub/f1/{name}")
        self.forge.add_pr(651, BR, ["ui/qt/src/x.cpp"])
        self.forge.tips = {BR: {"sha": "feat"},
                           "sub/f1/tests": {"sha": "tests"}}
        self.forge.ancestry = {("tests", "feat")}

    def test_stuck_feature_takes_the_pr_on_its_branch(self):
        o = self.make()
        self.feature(o)
        self.assertEqual(o.train.adopt(auto=True), [])
        f = o.store.get_feature("f1")
        self.assertEqual((f["pr"], f["state"]), (651, "pr_open"))
        self.assertEqual({t["state"] for t in o.store.feature_tasks("f1")},
                         {"pr_open"})
        self.assertFalse(o.train.is_adopted("pr-651"))
        self.assertEqual(
            [e["detail"] for e in o.store.events(kind="pr_linked")],
            ["#651 feat/f1 carries f1.tests"])

    def test_blocked_slice_not_in_the_pr_stays_blocked(self):
        o = self.make()
        self.feature(o)
        self.forge.ancestry = set()
        o.train.adopt(auto=True)
        self.assertEqual(o.store.get_task("f1.tests")["state"], "blocked")
        self.assertEqual(o.store.get_task("f1.ui")["state"], "pr_open")

    def test_feature_with_a_slice_still_building_is_left_alone(self):
        o = self.make()
        self.feature(o, tests_state="building")
        self.assertEqual(o.train.adopt(auto=True), [])
        self.assertIsNone(o.store.get_feature("f1")["pr"])

    def test_feature_with_an_agent_running_is_left_alone(self):
        o = self.make()
        self.feature(o)
        o.store.add_run(id="r1", task_id="f1.contract", role="integrator",
                        name="integrator", pid=0, started=o.clock())
        o.train.adopt(auto=True)
        self.assertIsNone(o.store.get_feature("f1")["pr"])

    def test_the_watch_merges_the_linked_pr_when_green(self):
        o = self.make()
        self.feature(o)
        self.forge.prs[651]["checks"] = {"Gate": "pass"}
        o.train.adopt(auto=True)
        o.tick()
        self.assertEqual(o.store.get_feature("f1")["state"], "merged")

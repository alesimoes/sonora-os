"""A stuck feature whose branch was merged under a train entry is closed
(ui.editing.commands, 2026-11-09: #651 was adopted by number as pr-651 and
merged there; the feature kept pr=None with its code on main)."""
import support

from harness import autoadopt, feature_pr_merged

BR = "feat/f1"
SUBS = ("contract", "ui", "tests")


class FeaturePrMerged(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def feature(self, o, tests_state="blocked", pr_state="merged"):
        o.store.add_feature("f1", "daw", branch=BR)
        for name in SUBS:
            st = tests_state if name == "tests" else "integrating"
            o.store.add_task(f"f1.{name}", "daw", "ui-engineer", state=st,
                             feature_id="f1", branch=f"sub/f1/{name}")
        # The entry the PR merged under, as Train.adopt leaves it.
        o.store.add_task("pr-651", "daw", "ui-engineer", state="merged",
                         branch=BR)
        o.store.set_task("pr-651", pr=651)
        o.store.set_feature("pr-651", pr=651, branch=BR, state="merged")
        self.forge.add_pr(651, BR, ["ui/qt/src/x.cpp"])
        self.forge.prs[651].update(state=pr_state, head="prhead")
        self.forge.tips = {f"sub/f1/{n}": {"sha": n} for n in SUBS}
        self.forge.ancestry = {(n, "prhead") for n in SUBS}

    def states(self, o):
        return {t["id"][3:]: t["state"] for t in o.store.feature_tasks("f1")}

    def test_feature_merged_under_an_entry_is_closed(self):
        o = self.make()
        self.feature(o)
        o.store.set_task("f1.ui", eng_name="eng-1")
        o.broker.claim_for("eng-1", ["ui/qt/src/x.cpp"])
        self.assertEqual(o.train.adopt(auto=True), [])   # not an open PR
        self.assertNotEqual(o.store.get_feature("f1")["state"], "merged")
        self.assertEqual(feature_pr_merged.sweep(o), ["f1"])
        self.assertEqual(o.store.get_feature("f1")["state"], "merged")
        self.assertEqual(set(self.states(o).values()), {"merged"})
        self.assertNotIn("eng-1", o.broker.claim_owners())
        self.assertEqual(
            [e["detail"] for e in o.store.events(kind="merged_elsewhere")],
            ["PR #651 as pr-651"])
        self.assertEqual(feature_pr_merged.sweep(o), [])

    def test_the_daemon_sweep_does_it(self):
        o = self.make()
        self.feature(o)
        autoadopt.tick(o, o.auto_adopt_at + 10000)
        self.assertEqual(o.store.get_feature("f1")["state"], "merged")

    def test_finished_slice_missing_from_the_pr_keeps_the_feature_open(self):
        o = self.make()
        self.feature(o)
        self.forge.ancestry.discard(("ui", "prhead"))
        self.assertEqual(feature_pr_merged.sweep(o), [])
        self.assertEqual(self.states(o), {
            "contract": "integrating", "ui": "integrating",
            "tests": "blocked"})

    def test_blocked_slice_missing_from_the_pr_stays_blocked(self):
        o = self.make()
        self.feature(o)
        self.forge.ancestry.discard(("tests", "prhead"))
        self.assertEqual(feature_pr_merged.sweep(o), ["f1"])
        self.assertEqual(self.states(o), {
            "contract": "merged", "ui": "merged", "tests": "blocked"})

    def test_entry_whose_pr_is_not_merged_on_github_closes_nothing(self):
        o = self.make()
        self.feature(o, pr_state="closed")
        self.assertEqual(feature_pr_merged.sweep(o), [])
        self.assertEqual(self.states(o)["ui"], "integrating")

    def test_feature_still_being_built_is_left_alone(self):
        o = self.make()
        self.feature(o, tests_state="building")
        self.assertEqual(feature_pr_merged.sweep(o), [])
        self.assertEqual(self.states(o)["ui"], "integrating")

    def test_feature_with_an_agent_running_is_left_alone(self):
        o = self.make()
        self.feature(o)
        o.store.add_run(id="r1", task_id="f1.contract", role="integrator",
                        name="integrator", pid=0, started=o.clock())
        self.assertEqual(feature_pr_merged.sweep(o), [])
        self.assertEqual(self.states(o)["ui"], "integrating")


if __name__ == "__main__":
    support.unittest.main()

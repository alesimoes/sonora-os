"""A PR opened from a slice's sub-branch carries the slice (ampcab.budget:
#669 from sub/ampcab.budget/budget-ir merged as entry pr-669 on 2026-11-12,
the feature sat in integrating with its slice blocked)."""
import support
from harness import feature_sync, remainder, slice_pr_link

FID = "ampcab.budget"


class SlicePrLink(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"plugin": 0})
        return super().make(**kw)

    def feature(self, o, slices=(("budget-ir", "blocked"),), owner=None):
        s = o.store
        s.add_feature(FID, "plugin", branch=f"feat/{FID}", partial=True)
        for name, st in slices:
            s.add_task(f"{FID}.{name}", "plugin", "dsp-engineer", state=st,
                       feature_id=FID, branch=f"sub/{FID}/{name}")
        s.set_task(f"{FID}.{slices[0][0]}", owner=owner,
                   note="PR #669 is this feature's own work")
        s.set_feature(FID, state="integrating")

    def entry(self, o, n=669, name="budget-ir", state="merged"):
        b = f"sub/{FID}/{name}"
        o.store.add_task(f"pr-{n}", "plugin", "dsp-engineer", state=state,
                         branch=b, brief="ampcab-budget: class-E budget")
        o.store.set_task(f"pr-{n}", pr=n)
        o.store.set_feature(f"pr-{n}", pr=n, branch=b, state=state)

    def states(self, o):
        return {t["id"].split(".")[-1]: t["state"]
                for t in o.store.feature_tasks(FID)}

    def test_merged_slice_pr_merges_the_feature(self):
        o = self.make()
        self.feature(o)
        self.entry(o)
        feature_sync.sync(o, o.clock())
        self.assertEqual(self.states(o), {"budget-ir": "merged"})
        f = o.store.get_feature(FID)
        self.assertEqual(f["state"], "merged")
        self.assertTrue(f["partial"])
        # and its rest is now continuable, with the entry's PR
        self.assertEqual(remainder.continuable(o.store)[0]["prs"], [669])

    def test_unfinished_siblings_leave_the_slice_alone(self):
        # a merged slice among integrating ones would keep the feature PR
        # from ever opening (_ready_features wants all integrating)
        o = self.make()
        self.feature(o, (("budget-ir", "integrating"), ("rerun", "building")))
        self.entry(o)
        self.assertEqual(slice_pr_link.sweep(o), [])
        self.assertEqual(self.states(o),
                         {"budget-ir": "integrating", "rerun": "building"})

    def test_finished_siblings_let_the_feature_merge(self):
        o = self.make()
        self.feature(o, (("budget-ir", "integrating"), ("adr", "closed")))
        self.entry(o)
        self.assertEqual(slice_pr_link.sweep(o), [f"{FID}.budget-ir"])
        self.assertEqual(o.store.get_feature(FID)["state"], "merged")

    def test_open_entry_moves_nothing(self):
        o = self.make()
        self.feature(o)
        self.entry(o, state="pr_open")
        self.assertEqual(slice_pr_link.sweep(o), [])
        self.assertEqual(self.states(o), {"budget-ir": "blocked"})

    def test_owner_blocked_slice_stays_blocked(self):
        o = self.make()
        self.feature(o, owner="pick the IR length")
        self.entry(o)
        self.assertEqual(slice_pr_link.sweep(o), [])
        self.assertEqual(self.states(o), {"budget-ir": "blocked"})

    def test_slice_still_worked_on_is_not_moved(self):
        o = self.make()
        self.feature(o, (("budget-ir", "building"),))
        self.entry(o)
        self.assertEqual(slice_pr_link.sweep(o), [])

    def test_feature_branch_entries_are_not_this_shape(self):
        o = self.make()
        self.feature(o)
        o.store.add_task("pr-670", "plugin", "x", state="merged",
                         branch=f"feat/{FID}")
        o.store.set_feature("pr-670", pr=670, branch=f"feat/{FID}",
                            state="merged")
        self.assertEqual(slice_pr_link.sweep(o), [])


if __name__ == "__main__":
    import unittest
    unittest.main()

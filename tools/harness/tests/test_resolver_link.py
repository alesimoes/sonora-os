"""A PR a stall resolver opened from its own branch closes its feature
(#687 ui.comping.lanes, #688 aaf_export: merged as train entries pr-N while
the features sat in building with stall alerts, 2026-11-13)."""
import json
import os

import support
from harness import feature_sync, resolver_link, stall_check

FID = "ui.lanes"


class ResolverLink(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def feature(self, o):
        s = o.store
        s.add_feature(FID, "daw", branch=f"feat/{FID}", partial=True)
        for name, st in (("adr", "integrating"), ("model", "blocked"),
                         ("tests", "queued")):
            s.add_task(f"{FID}.{name}", "daw", "ui-engineer", state=st,
                       feature_id=FID, branch=f"sub/{FID}/{name}")
        s.set_feature(FID, state="building")

    def resolver(self, o, result, n=691):
        """A finished stall resolver run whose result.json is `result`."""
        wid = f"wake-principal-stall_resolver-{FID}-{n}"
        o.store.add_task(wid, None, "principal-engineer", kind="wake",
                         feature_id=FID, state="merged")
        o.store.set_task(wid, event="stall_resolver")
        path = os.path.join(self.tmp, f"result-{n}.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(dict({"status": "done", "summary": "x"}, **result), f)
        o.store.add_run(id=f"{wid}-r1", task_id=wid, role="planner",
                        name="principal-engineer", state="x", pid=None,
                        started=1.0, last_beat=1.0, result_path=path,
                        stream_path="y", worktree=None)
        o.store.set_run(f"{wid}-r1", ended=2.0, outcome="finished")

    def entry(self, o, n, branch, title, state="merged"):
        """The train entry Train.adopt makes for an adopted PR."""
        o.store.add_task(f"pr-{n}", "daw", "ui-engineer", state=state,
                         branch=branch, brief=title)
        o.store.set_task(f"pr-{n}", pr=n)
        o.store.set_feature(f"pr-{n}", pr=n, branch=branch, state=state)

    def states(self, o):
        return {t["id"].split(".")[-1]: t["state"]
                for t in o.store.feature_tasks(FID)}

    def test_branch_reported_by_the_resolver_closes_the_feature(self):
        o = self.make()
        self.feature(o)
        self.resolver(o, {"branch": "fix/lanes-691"})
        self.entry(o, 687, "fix/lanes-691", "squash of the lanes")
        self.assertEqual(resolver_link.sweep(o), [FID])
        self.assertEqual(o.store.get_feature(FID)["state"], "merged")
        self.assertEqual(self.states(o), {"adr": "merged", "model": "closed",
                                          "tests": "closed"})
        self.assertEqual(resolver_link.sweep(o), [])   # idempotent

    def test_title_naming_the_feature_closes_it(self):
        o = self.make()
        self.feature(o)
        self.resolver(o, {})
        self.entry(o, 688, "fix/other", f"[partial] {FID}: export")
        self.assertEqual(resolver_link.sweep(o), [FID])

    def test_title_alone_does_not_close_a_feature_never_stalled(self):
        o = self.make()
        self.feature(o)
        self.entry(o, 690, "fix/contract", f"{FID}: contract first")
        self.assertEqual(resolver_link.sweep(o), [])
        self.assertEqual(o.store.get_feature(FID)["state"], "building")

    def test_open_entry_pr_is_not_merged_but_stops_stall_alerts(self):
        o = self.make()
        self.feature(o)
        self.resolver(o, {"branch": "fix/lanes-691"})
        self.entry(o, 687, "fix/lanes-691", "x", state="pr_open")
        self.assertEqual(resolver_link.sweep(o), [])
        self.assertEqual(o.store.get_feature(FID)["state"], "building")
        t = o.store.get_task(f"{FID}.adr")
        self.assertTrue(stall_check.waits_elsewhere(o.store, t))

    def test_owner_blocked_slice_stays_blocked_and_visible(self):
        o = self.make()
        self.feature(o)
        o.store.set_task(f"{FID}.model", owner="choose a writer backend")
        self.resolver(o, {"branch": "fix/lanes-691"})
        self.entry(o, 687, "fix/lanes-691", "x")
        resolver_link.sweep(o)
        self.assertEqual(self.states(o)["model"], "blocked")
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        self.assertEqual([w["task"] for w in st["waits_on_owner"]],
                         [f"{FID}.model"])

    def test_the_daemon_tick_does_it(self):
        o = self.make()
        self.feature(o)
        self.resolver(o, {"branch": "fix/lanes-691"})
        self.entry(o, 687, "fix/lanes-691", "x")
        feature_sync.sync(o, o.clock())
        self.assertEqual(o.store.get_feature(FID)["state"], "merged")

    def test_resolver_own_feature_branch_is_not_recorded(self):
        o = self.make()
        self.feature(o)
        self.assertFalse(resolver_link.record(
            o.store, FID, {"branch": f"sub/{FID}/adr"}))
        self.assertTrue(resolver_link.record(o.store, FID, {"pr": 9}))

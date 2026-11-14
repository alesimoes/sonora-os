"""A stall resolver's re-plan takes effect (ui.record.punch_loop: six
re-plans in two days changed nothing, 83 h in integrating, 2026-11-13)."""
import json
import os

import support
from harness import feature_sync, replan

FID = "ui.punch"


class Replan(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        o = super().make(**kw)
        self.ingested = []

        def fake_ingest(fid):
            self.ingested.append(fid)
            o.store.set_feature(fid, state="distributing")
            return True, []
        o.flow.ingest_plan = fake_ingest
        return o

    def feature(self, o):
        s = o.store
        s.add_feature(FID, "daw", branch=f"feat/{FID}")
        s.add_task(f"{FID}.ui", "daw", "ui-engineer", state="integrating",
                   feature_id=FID)
        s.add_task(f"{FID}.tests", "daw", "ui-engineer", state="blocked",
                   feature_id=FID)
        s.set_task(f"{FID}.tests", note="the fix is outside my claim")
        s.set_feature(FID, state="building")

    def wake(self, o, agent="architect"):
        wid = f"wake-{agent}-stall_resolver-{FID}-9"
        o.store.add_task(wid, None, agent, kind="wake", feature_id=FID)
        o.store.set_task(wid, event="stall_resolver")
        return o.store.get_task(wid)

    def write(self, o, plan, accepted=None):
        os.makedirs(os.path.dirname(o.flow.plan_file(FID)), exist_ok=True)
        with open(o.flow.plan_file(FID), "w") as f:
            json.dump(plan, f)
        if accepted is not None:
            p = o.flow.accepted_plan_file(FID)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w") as f:
                json.dump(accepted, f)

    def states(self, o):
        return {t["id"]: t["state"] for t in o.store.feature_tasks(FID)}

    def test_new_plan_retires_old_slices_and_starts_a_generation(self):
        o = self.make()
        self.feature(o)
        self.write(o, {"tasks": [{"id": "ui"}]}, accepted={"tasks": [1, 2]})
        ok, errs = o.flow.on_wake_done(self.wake(o))
        self.assertEqual((ok, errs), (True, []))
        self.assertEqual(self.ingested, [FID])
        self.assertEqual(set(self.states(o).values()), {"closed"})
        self.assertEqual(o.flow._task_id(FID, "ui"), f"{FID}.ui-r1")
        # the lead's table now creates fresh ids, so the feature is not
        # closed by feature_sync while it is being distributed
        feature_sync.sync(o, o.clock())
        self.assertEqual(o.store.get_feature(FID)["state"], "distributing")

    def test_unchanged_plan_hands_the_blocked_slice_to_the_owner(self):
        o = self.make()
        self.feature(o)
        self.write(o, {"tasks": [1]}, accepted={"tasks": [1]})
        self.assertEqual(o.flow.on_wake_done(self.wake(o)), (True, []))
        self.assertEqual(self.ingested, [])
        t = o.store.get_task(f"{FID}.tests")
        self.assertIn("re-plan left the plan unchanged", t["owner"])

    def test_empty_plan_closes_the_feature(self):
        o = self.make()
        self.feature(o)
        self.write(o, {"tasks": []}, accepted={"tasks": [1]})
        o.flow.on_wake_done(self.wake(o))
        self.assertEqual(o.store.get_feature(FID)["state"], "closed")

    def test_missing_plan_file_is_rejected_with_the_reason(self):
        o = self.make()
        self.feature(o)
        ok, errs = o.flow.on_wake_done(self.wake(o))
        self.assertFalse(ok)
        self.assertIn("not only the markdown plan", errs[0])

    def test_waits_for_a_running_slice_then_applies_on_the_tick(self):
        o = self.make()
        self.feature(o)
        self.write(o, {"tasks": [{"id": "ui"}]}, accepted={"tasks": []})
        o.store.add_run(id="r1", task_id=f"{FID}.ui", role="integrator",
                        name="integrator", state="x", pid=None, started=1.0,
                        last_beat=1.0, result_path="x", stream_path="y",
                        worktree=None)
        o.flow.on_wake_done(self.wake(o))
        self.assertEqual(self.ingested, [])
        replan.apply_pending(o)
        self.assertEqual(self.ingested, [])
        o.store.set_run("r1", ended=2.0)
        replan.apply_pending(o)
        self.assertEqual(self.ingested, [FID])
        replan.apply_pending(o)
        self.assertEqual(self.ingested, [FID])    # once

    def test_principal_resolver_wake_is_not_a_replan(self):
        o = self.make()
        self.feature(o)
        self.assertEqual(o.flow.on_wake_done(self.wake(o, "principal-"
                                                       "engineer")),
                         (True, []))
        self.assertEqual(self.ingested, [])

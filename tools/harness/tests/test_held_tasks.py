"""A task held until another feature merges (not_before=HOLD) is not work
the track can start. 2026-11-13: ui.comping.exit_test.exit-checklist was
held on ui.comping.consolidate (closed: already on main); counting it as
eligible read "files claimed or dependencies pending" and the lead was never
woken, so the DAW track sat at 0 of 8 engineers for hours."""
import json
import os
import unittest

import support  # noqa: F401  (sets sys.path)
from harness.scheduler import HOLD
from test_daemon_unit import Base, plan
import test_supervisor as ts

H = 3600.0


class SchedulerTests(Base):
    def held(self, tid="h.a", track="daw"):
        self.s.add_task(tid, track, "ui-engineer", feature_id="h",
                        files=[f"ui/{tid}/**"])
        self.s.set_task(tid, not_before=HOLD)

    def test_held_task_alone_wakes_the_lead_under_floor(self):
        self.held()
        p = plan(self.s)
        self.assertIn(("lead", "pool_under_floor", "daw"), p.wakes)
        self.assertIn("another feature", p.gaps["daw"])
        self.assertNotIn("claimed", p.gaps["daw"])
        self.assertEqual([], [x for x in p.launches if x.kind == "engineer"])

    def test_eligible_task_refused_a_claim_still_suppresses_the_wake(self):
        # Two queued slices on one file: one launches, the other is eligible
        # but refused this tick; work is there, the lead is not needed.
        self.held()
        self.s.add_task("one", "daw", "ui-engineer", files=["ui/shared/**"])
        self.s.add_task("two", "daw", "ui-engineer", files=["ui/shared/**"])
        p = plan(self.s)
        self.assertEqual(["one"], [x.task_id for x in p.launches
                                   if x.kind == "engineer"])
        self.assertNotIn(("lead", "pool_under_floor", "daw"), p.wakes)
        self.assertEqual("files claimed or dependencies pending",
                         p.gaps["daw"])

    def test_track_without_any_task_still_reads_no_eligible_task(self):
        p = plan(self.s)
        self.assertEqual("no eligible task", p.gaps["daw"])
        self.assertIn(("lead", "pool_under_floor", "daw"), p.wakes)


class StaleHoldAlerts(ts.Case):
    def setUp(self):
        super().setUp()
        self.sv.th.stall_alert_s = 2 * H

    def feature_held_on(self, dep):
        s = self.o.store
        s.add_feature("w", "daw", branch="feat/w", plan_path="p.md")
        s.add_task("w.a", "daw", "ui-engineer", feature_id="w")
        s.set_task("w.a", not_before=HOLD)
        path = self.o.flow.accepted_plan_file("w")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"depends_on_feature": dep, "tasks": []}, f)

    def test_hold_on_a_closed_feature_is_an_alert(self):
        self.o.store.add_feature("dep", "daw")
        self.o.store.set_feature("dep", state="closed")
        self.feature_held_on("dep")
        a = self.sweep()["hold:w"]
        self.assertIn("dep", a["text"])
        self.assertIn("closed", a["text"])
        self.assertIn("w.a", a["text"])

    def test_hold_on_a_missing_feature_is_an_alert(self):
        self.feature_held_on("nowhere")
        self.assertIn("not in the store", self.sweep()["hold:w"]["text"])

    def test_hold_on_an_open_feature_is_not_an_alert(self):
        self.o.store.add_feature("dep", "daw")
        self.o.store.set_feature("dep", state="building")
        self.feature_held_on("dep")
        self.assertNotIn("hold:w", self.sweep())


if __name__ == "__main__":
    unittest.main()

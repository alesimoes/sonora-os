"""A feature held by one blocked slice is routed and says who holds it.

2026-11-13: clip_groups.rest, plugins.host.editors.rest and live_loops.rest
had finished slices in `integrating` for hours, no integrator (it waits for
every live slice) and a blocked sibling nobody acted on: each feature also
had a resolver wake blocked with "restart Docker" / "re-queue the
integrator" read as an owner ask, which made stall_check skip the feature
as "waits on the owner". The status said nothing about which slice held it.
"""
import json
import os
import unittest

import support  # noqa: F401
from harness import stall_resolver as SR
from harness.supervisor import Thresholds
import test_supervisor as ts

H = 3600.0
DOCKER_ASK = "Owner/host action: restart Docker engine."


class Routed(ts.Case):
    def setUp(self):
        super().setUp()
        self.th = Thresholds(interval_s=0, notify_repeat_s=600,
                             stall_alert_s=2 * H, pr_stall_s=3 * H)
        self.sv = ts.Supervisor(self.o, self.sys, th=self.th,
                                clock=self.clock)
        self.o.supervisor = self.sv

    def age(self, hours):
        self.clock.offset += hours * H

    def resolvers(self):
        return [w for w in self.o.store.tasks(kind="wake")
                if w.get("event") == SR.EVENT]

    def held(self):
        s = self.o.store
        s.add_feature("f", "daw", branch="feat/f")
        s.add_task("f.wire", "daw", "rt-engineer", state="integrating",
                   feature_id="f", files=["w/**"])
        s.add_task("f.ui", "daw", "ui-engineer", state="blocked",
                   feature_id="f", files=["u/**"])
        s.set_task("f.ui", note="Build exceeded 10 min in the build slot "
                   "queue and was killed", cause="code")
        old = "wake-principal-stall_resolver-f-1"
        s.add_task(old, None, SR.AGENT, kind="wake", feature_id="f",
                   state="blocked")
        s.set_task(old, event=SR.EVENT, owner=DOCKER_ASK,
                   note="Docker Desktop died")

    def test_a_false_owner_ask_no_longer_keeps_the_resolver_away(self):
        self.held()
        self.age(2.5)
        al = self.sweep()
        queued = [w for w in self.resolvers() if w["state"] == "queued"]
        self.assertEqual(len(queued), 1)
        self.assertIn("Blocked sibling f.ui", queued[0]["brief"])
        self.assertIn("integrator waits for f.ui (blocked",
                      al["stall:f.wire"]["text"])

    def test_status_names_the_slice_that_holds_the_feature(self):
        self.held()
        self.o.tick()
        with open(os.path.join(self.o.odir, "status.json")) as f:
            st = json.load(f)
        feat = next(x for x in st["features"] if x["id"] == "f")
        self.assertEqual([(h["task"], h["state"], h["cause"])
                          for h in feat["held_by"]],
                         [("f.ui", "blocked", "code")])
        self.assertIn("build slot", feat["held_by"][0]["note"])

    def test_nothing_holds_a_feature_whose_slices_all_work(self):
        s = self.o.store
        s.add_feature("g", "daw", branch="feat/g")
        s.add_task("g.a", "daw", "rt-engineer", state="building",
                   feature_id="g", files=["a/**"])
        self.o.tick()
        with open(os.path.join(self.o.odir, "status.json")) as f:
            st = json.load(f)
        feat = next(x for x in st["features"] if x["id"] == "g")
        self.assertEqual(feat["held_by"], [])

    def test_replan_brief_says_what_to_do_with_a_blocked_slice(self):
        self.held()
        wid = SR.queue_replan(self.o, "f", "detail line", "sonnet")
        brief = self.o.store.get_task(wid)["brief"]
        self.assertIn("detail line", brief)
        self.assertIn("blocked slice", brief)
        self.assertIn("hardware or an owner decision", brief)


if __name__ == "__main__":
    unittest.main()

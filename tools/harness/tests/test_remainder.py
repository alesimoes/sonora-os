"""The rest of a partially merged feature can be continued (2026-11-13: 45
features merged `[partial]`, every proposal of one was rejected as "exists:
merged", and the pools idled with "no eligible task")."""
import json
import os
import unittest

import support
from harness import blockinfo, pool_prompt, remainder

ROOT = "ui.windows_nightly_tests"
REST = ROOT + ".rest"
WAKE = "wake-lead-pool_under_floor-daw-7"


class Ids(unittest.TestCase):
    def test_round_trip(self):
        self.assertEqual(remainder.rest_id(ROOT, 1), REST)
        self.assertEqual(remainder.rest_id(ROOT, 3), ROOT + ".rest3")
        self.assertEqual(remainder.root_of(ROOT + ".rest2"), ROOT)
        self.assertEqual(remainder.round_of(ROOT), 0)
        self.assertEqual(remainder.round_of(REST), 1)
        self.assertEqual(remainder.round_of(ROOT + ".rest3"), 3)

    def test_track_of_the_wake(self):
        self.assertEqual(pool_prompt.track_of(WAKE), "daw")
        self.assertIsNone(pool_prompt.track_of("wake-lead-qa_fail-x-1"))


class Remainder(support.DaemonCase):
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

    def merged(self, o, fid=ROOT, pr=674, partial=True, track="daw",
               ids=None):
        s = o.store
        s.add_feature(fid, track, branch=f"feat/{fid}", partial=partial)
        s.set_meta(f"feature:{fid}", json.dumps(
            {"backlog_ids": ids or [ROOT], "track": track}))
        s.set_feature(fid, state="merged", pr=pr, adr_no=160)

    def propose(self, o, fid=ROOT, ids=None):
        return o.flow.on_proposal({"feature": fid, "track": "daw",
                                   "backlog_ids": ids or [ROOT]}, "lead")

    def plan_wake(self, o, fid=REST):
        wid = f"wake-architect-plan_request-{fid}-3"
        o.store.add_task(wid, None, "architect", kind="wake", feature_id=fid)
        o.store.set_task(wid, event="plan_request")
        return o.store.get_task(wid)

    def write_plan(self, o, plan, fid=REST):
        os.makedirs(os.path.dirname(o.flow.plan_file(fid)), exist_ok=True)
        with open(o.flow.plan_file(fid), "w") as f:
            json.dump(plan, f)

    def lead_prompt(self, o, wid=WAKE):
        o.store.add_task(wid, None, "lead", kind="wake")
        o.store.set_task(wid, event="pool_under_floor")
        return o.flow.wake_context(o.store.get_task(wid))

    # -- offered -------------------------------------------------------
    def test_lead_prompt_lists_the_partial_feature_with_its_pr(self):
        o = self.make()
        self.merged(o)
        self.merged(o, "time.groove", pr=600, partial=False)
        self.merged(o, "drivers.sync.ltc", pr=618, track="os")
        text = self.lead_prompt(o)
        self.assertIn(f"- {ROOT} (daw, merged as #674): propose feature "
                      f"{REST}", text)
        self.assertNotIn("- time.groove", text)
        self.assertNotIn("- drivers.sync.ltc", text)   # another track's pool

    def test_out_of_v1_partial_is_not_offered(self):
        o = self.make()
        self.merged(o, "logic.midi_learn", ids=["logic.midi_learn"])
        self.assertEqual(remainder.continuable(o.store), [])

    def test_proposal_of_the_root_opens_the_remainder(self):
        o = self.make()
        self.merged(o)
        self.assertTrue(self.propose(o))
        f = o.store.get_feature(REST)
        self.assertEqual((f["state"], f["track"], f["branch"]),
                         ("queued", "daw", f"feat/{REST}"))
        info = o.flow.info(REST)
        self.assertEqual(info["backlog_ids"], [ROOT])
        self.assertEqual((info["continues"], info["prs"]), (ROOT, [674]))
        self.assertEqual(info["wants"], [])
        # opened once: the link is live, so it is no longer offered and a
        # second proposal is the usual "exists" rejection
        self.assertEqual(remainder.continuable(o.store), [])
        self.assertFalse(self.propose(o))
        self.assertFalse(self.propose(o, REST))

    def test_a_rest_of_nothing_is_rejected(self):
        o = self.make()
        self.merged(o, partial=False)
        self.assertFalse(self.propose(o, REST))
        self.assertIsNone(o.store.get_feature(REST))
        self.assertFalse(self.propose(o, "x.y.rest", ["x.y"]))

    def test_no_mass_opening_on_the_tick(self):
        o = self.make()
        self.merged(o)
        o.flow.tick(o.clock())
        self.assertIsNone(o.store.get_feature(REST))

    # -- planned -------------------------------------------------------
    def test_architect_brief_names_the_pr_and_the_old_plan(self):
        o = self.make()
        self.merged(o)
        self.propose(o)
        o.flow.request_plan(o.store.get_feature(REST), o.clock())
        text = o.flow.wake_context(self.plan_wake(o))
        self.assertIn("REMAINDER (round 1 of at most 3)", text)
        self.assertIn("PR #674", text)
        self.assertIn(f"planning/plans/{ROOT}.md", text)
        self.assertIn("plan ONLY what is still open", text)
        self.assertIn("needs_owner", text)
        self.assertIn("ADR-0160", text)
        self.assertEqual(o.flow.reserved_for(REST)["adr"], None)

    def test_empty_plan_completes_the_chain(self):
        o = self.make()
        self.merged(o)
        self.propose(o)
        self.write_plan(o, {"tasks": []})
        self.assertEqual(o.flow.on_wake_done(self.plan_wake(o)), (True, []))
        self.assertEqual(self.ingested, [])
        self.assertEqual(o.store.get_feature(REST)["state"], "closed")
        self.assertFalse(o.store.get_feature(ROOT)["partial"])
        self.assertEqual(remainder.continuable(o.store), [])

    def test_a_real_plan_is_validated_like_any_other(self):
        o = self.make()
        self.merged(o)
        self.propose(o)
        self.write_plan(o, {"tasks": [{"id": "cmake"}]})
        self.assertEqual(o.flow.on_wake_done(self.plan_wake(o)), (True, []))
        self.assertEqual(self.ingested, [REST])

    def test_owner_decision_goes_to_waits_on_owner(self):
        o = self.make()
        self.merged(o)
        self.propose(o)
        w = self.plan_wake(o)
        o._on_agent_blocked(w, "planner", {
            "summary": "the font licence is open",
            "needs_owner": "pick the bundled font"}, o.clock())
        self.assertEqual(o.store.get_feature(REST)["state"], "blocked")
        items = blockinfo.owner_items(
            o.store.tasks(), {f["id"]: f for f in o.store.features()})
        self.assertEqual([i["task"] for i in items], [w["id"]])
        self.assertEqual(remainder.continuable(o.store), [])

    # -- settled -------------------------------------------------------
    def test_remainder_merged_whole_clears_partial(self):
        o = self.make()
        self.merged(o)
        self.propose(o)
        o.store.set_feature(REST, state="merged", pr=701, partial=0)
        o.flow.tick(o.clock())
        self.assertFalse(o.store.get_feature(ROOT)["partial"])
        self.assertEqual(remainder.continuable(o.store), [])

    def test_remainder_merged_partial_continues_until_the_bound(self):
        o = self.make()
        self.merged(o)
        for n, pr in ((1, 701), (2, 702), (3, 703)):
            rid = remainder.rest_id(ROOT, n)
            self.assertEqual([r["next"] for r in
                              remainder.continuable(o.store)], [rid])
            self.assertTrue(self.propose(o))
            o.store.set_feature(rid, state="merged", pr=pr, partial=1)
            o.flow.tick(o.clock())
            self.assertTrue(o.store.get_feature(ROOT)["partial"])
        self.assertEqual(remainder.continuable(o.store), [])
        self.assertFalse(self.propose(o))
        self.assertIn("Partial after 3 remainders", self.lead_prompt(o))
        self.assertIn(ROOT, remainder.spent(o.store))

    def test_remainder_closed_unmerged_can_be_reopened(self):
        o = self.make()
        self.merged(o)
        self.propose(o)
        o.store.set_feature(REST, state="closed")
        self.assertEqual([r["next"] for r in remainder.continuable(o.store)],
                         [ROOT + ".rest2"])

    # -- parked and out of scope (owner, 2026-11-13) ------------------
    def parked(self, o):
        self.merged(o)
        self.propose(o)
        w = self.plan_wake(o)
        o._on_agent_blocked(w, "planner", {
            "summary": "Only the hardware power-pull run remains.",
            "needs_owner": "Run the power-pull protocol on the UMC1820 "
                           "hardware unit."}, o.clock())
        # what the coordinator does with a stale blocked remainder
        o.store.set_feature(REST, state="closed")

    def test_a_hardware_only_remainder_is_never_offered_again(self):
        o = self.make()
        self.parked(o)
        self.assertEqual(remainder.continuable(o.store), [])
        self.assertEqual(remainder.parked(o.store), [ROOT])
        text = self.lead_prompt(o)
        self.assertNotIn(f"propose feature {ROOT}.rest2", text)
        self.assertIn("Parked for a hardware run", text)
        self.assertIn(ROOT, text.split("Parked for a hardware run")[1])
        self.assertFalse(self.propose(o))
        self.assertIsNone(o.store.get_feature(ROOT + ".rest2"))

    def test_aaf_export_and_round_trip_are_out_of_v1(self):
        o = self.make()
        for fid in ("interchange.sessions.aaf_export",
                    "interchange.sessions.roundtrip",
                    "interchange.sessions.roundtrip.rest"):
            self.assertFalse(self.propose(o, fid, [fid]), fid)
            self.assertIsNone(o.store.get_feature(fid))
        self.merged(o, "interchange.sessions.roundtrip",
                    ids=["interchange.sessions.roundtrip"])
        self.assertEqual(remainder.continuable(o.store), [])
        self.assertTrue(self.propose(o, "interchange.sessions.aaf_import2",
                                     ["interchange.sessions.aaf_import"]))
        self.assertIn("AAF export", self.lead_prompt(o))

    def test_pr_of_a_root_merged_by_a_train_entry(self):
        o = self.make()
        self.merged(o, pr=None)
        o.store.add_task("pr-669", "daw", "x", state="merged",
                         branch=f"sub/{ROOT}/t1")
        o.store.set_feature("pr-669", pr=669, branch=f"sub/{ROOT}/t1",
                            state="merged")
        self.assertEqual(remainder.continuable(o.store)[0]["prs"], [669])


if __name__ == "__main__":
    unittest.main()

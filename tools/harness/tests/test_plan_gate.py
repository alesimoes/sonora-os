"""Plan gate and automatic adoption (owner, 2026-11-06)."""
import json
import os
import unittest

import support
import test_feature_flow as ff
from harness import autoadopt, plangate, routing
from test_routing_tiers import TOML, write_repo


class PlanFileCheck(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.d = tempfile.mkdtemp()
        write_repo(self.d)
        self.r = routing.load(self.d)
        os.makedirs(os.path.join(self.d, "planning", "plans"))

    def tearDown(self):
        import shutil
        shutil.rmtree(self.d, ignore_errors=True)

    def plan(self, text):
        with open(os.path.join(self.d, "planning", "plans", "f1.md"),
                  "w") as f:
            f.write(text)

    def test_missing_file_is_refused(self):
        self.assertTrue(plangate.check(self.d, "f1", self.r))

    def test_marker_must_be_a_line_of_its_own(self):
        self.plan("# Plan\nStatus: draft\nwrite `Status: approved` later\n")
        self.assertTrue(plangate.check(self.d, "f1", self.r))
        self.plan("# Plan\n\nStatus: approved\n")
        self.assertEqual(plangate.check(self.d, "f1", self.r), [])
        self.plan("# Plan\r\n- status: Approved  \r\n")
        self.assertEqual(plangate.check(self.d, "f1", self.r), [])

    def test_gate_is_off_without_routing_toml(self):
        self.assertEqual(plangate.check(self.d, "nope", None), [])


class GatedFlow(ff.FlowCase):
    def make(self, **kw):
        write_repo(self.tmp, TOML)
        return super().make(**kw)

    def approve(self):
        d = os.path.join(self.tmp, "planning", "plans")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, f"{ff.FEATURE}.md"), "w") as f:
            f.write("# Plan\n\nStatus: approved\n")

    def test_no_engineer_without_an_approved_plan(self):
        o = self.make()
        self.script("architect", "architect")
        self.script("lead", "lead")
        self.open(o)
        self.assertTrue(self.pump(
            o, lambda: o.store.events(kind="plan_not_approved"), timeout=30))
        self.assertEqual(o.store.feature_tasks(ff.FEATURE), [])
        self.assertNotEqual(ff.fstate(o), "building")
        self.approve()
        self.assertTrue(self.pump(o, lambda: ff.fstate(o) == "building",
                                  timeout=60))
        self.assertEqual(len(o.store.feature_tasks(ff.FEATURE)), 3)
        # the lead was woken after approval, as before
        self.assertTrue(ff.prompts("lead"))
        self.assertIn("planning/plans/", "\n".join(ff.prompts("architect")))

    def test_tier_in_the_plan_reaches_the_task(self):
        o = self.make()
        self.approve()
        plan = ff.canned_plan()
        plan["tasks"][1]["tier"] = "senior"
        plan["tasks"][2]["grade"] = "S/low"
        self.plan(ff.FEATURE, plan)
        self.script("architect", "architect")
        self.script("lead", "lead")
        self.open(o)
        self.assertTrue(self.pump(o, lambda: ff.fstate(o) == "building",
                                  timeout=60))
        from harness import dispatch
        self.assertEqual(dispatch.tier_of(o, f"{ff.FEATURE}.t2"),
                         ("senior", None))
        self.assertEqual(dispatch.tier_of(o, f"{ff.FEATURE}.t3"),
                         (None, "S/low"))


class AutoAdopt(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def events(self, o):
        return [e["detail"] for e in o.store.events(kind="adopted_pr")]

    def test_open_non_draft_prs_against_main_are_adopted_by_the_tick(self):
        o = self.make()
        self.forge.add_pr(10, "a", ["engine/x.cpp"])
        self.forge.add_pr(11, "b", ["engine/y.cpp"], draft=True)
        self.forge.add_pr(12, "c", ["engine/z.cpp"], base="feat/other")
        o.tick()
        # The stacked one (#12) is adopted too, to be retargeted to main
        # (ADR-0061 section 6); only the draft stays out.
        self.assertEqual(sorted(self.events(o)), ["#10 a auto", "#12 c auto"])
        self.assertTrue(o.train.is_adopted("pr-10"))
        self.assertFalse(o.train.is_adopted("pr-11"))
        self.assertTrue(o.train.is_adopted("pr-12"))
        self.assertEqual(self.forge.prs[12]["base"], "main")

    def test_sweep_follows_the_refresh_cadence_and_never_twice(self):
        o = self.make()
        o.tick()
        self.forge.add_pr(10, "a", ["engine/x.cpp"])
        o.tick()
        self.assertEqual(self.events(o), [])     # inside the cadence
        self.clock.offset += 400
        o.tick()
        self.assertEqual(self.events(o), ["#10 a auto"])
        self.clock.offset += 400
        o.tick()
        self.assertEqual(len(self.events(o)), 1)

    def test_a_ready_draft_is_taken_once_it_leaves_draft(self):
        o = self.make()
        self.forge.add_pr(11, "b", ["engine/y.cpp"], draft=True)
        o.tick()
        self.assertEqual(self.events(o), [])
        self.forge.prs[11]["draft"] = False
        self.clock.offset += 400
        o.tick()
        self.assertEqual(self.events(o), ["#11 b auto"])

    def test_dry_run_adopts_nothing(self):
        o = self.make(dry_run=True)
        self.forge.add_pr(10, "a")
        o.tick()
        self.assertEqual(self.events(o), [])


class AdoptRequest(support.DaemonCase):
    def test_request_file_is_taken_by_the_tick_not_written_by_the_cli(self):
        o = self.make(floors={"daw": 0}, dry_run=True)   # no auto sweep
        self.forge.add_pr(10, "a")
        self.forge.add_pr(11, "b")
        autoadopt.write_request(o.hdir, [11])
        self.assertFalse(o.train.is_adopted("pr-11"))
        o.tick()
        self.assertTrue(o.train.is_adopted("pr-11"))
        self.assertFalse(o.train.is_adopted("pr-10"))
        self.assertEqual(os.listdir(autoadopt.request_dir(o.hdir)), [])

    def test_empty_request_means_every_open_pr(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        self.forge.add_pr(10, "a")
        self.forge.add_pr(11, "b", draft=True)
        autoadopt.write_request(o.hdir, [])
        o.tick()
        self.assertTrue(o.train.is_adopted("pr-10"))
        self.assertTrue(o.train.is_adopted("pr-11"))   # asked for by hand

    def test_cli_queues_while_the_daemon_runs(self):
        import contextlib
        import io
        odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(odir)
        with open(os.path.join(odir, "status.json"), "w") as f:
            json.dump({}, f)
        self.assertTrue(autoadopt.daemon_alive(odir))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = support.orchestrator.main(["--root", self.tmp, "adopt",
                                            "7", "8"])
        self.assertEqual(rc, 0)
        self.assertIn("queued", out.getvalue())
        self.assertEqual(autoadopt.take_requests(
            os.path.join(self.tmp, ".harness")), [{7, 8}])
        self.assertFalse(os.path.exists(os.path.join(odir, "state.json")))

    def test_stale_status_means_no_daemon(self):
        odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(odir)
        p = os.path.join(odir, "status.json")
        with open(p, "w") as f:
            f.write("{}")
        old = os.path.getmtime(p) - 3600
        os.utime(p, (old, old))
        self.assertFalse(autoadopt.daemon_alive(odir))
        self.assertFalse(autoadopt.daemon_alive(self.tmp))


class SeniorBeforePrincipal(support.DaemonCase):
    """The same red: fix rounds, then one senior-tier round of the area
    agent, then the principal engineer (fable)."""

    def test_senior_round_then_principal(self):
        from harness import dispatch
        write_repo(self.tmp, TOML)
        o = super().make(floors={"daw": 0}, principal_after_fixes=1)
        self.forge.add_pr(10, "a", ["engine/x.cpp"])
        o.train.adopt()
        f = o.store.get_feature("pr-10")
        ts = o.store.feature_tasks("pr-10")
        o._red(f, ts, ["Engine"], "ci")
        f = o.store.get_feature("pr-10")
        self.assertFalse(dispatch.senior_round_active(o, ts[0]))
        self.assertFalse(f.get("principal_runs"))
        o._red(f, ts, ["Engine"], "ci")              # stuck: senior round
        f = o.store.get_feature("pr-10")
        self.assertTrue(dispatch.senior_round_active(o, ts[0]))
        self.assertFalse(f.get("principal_runs"))
        self.assertEqual(dispatch.model_for(o, ts[0], "fixer"), "opus")
        o._red(f, ts, ["Engine"], "ci")              # still red: principal
        f = o.store.get_feature("pr-10")
        self.assertEqual(f.get("principal_runs"), 1)
        self.assertTrue(o.store.get_task(ts[0]["id"])["escalated"])


if __name__ == "__main__":
    unittest.main()

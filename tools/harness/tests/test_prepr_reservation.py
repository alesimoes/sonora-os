"""pre-pr.sh in the daemon (ui.mixer.sidechain blocked 4 rounds, 2026-11-12):
the protocol allowance follows the store's reservation and nothing else, the
FAIL lines survive the cut, and a plan that changes a version constant gets
its number reserved when it is ingested."""
import os
import subprocess
import tempfile
import unittest

import support
import test_feature_flow
from harness import forge, plan_numbers, prepr

WARN = "pre-pr: warning: engine/x.cpp has 900 lines (+3 here); split it"


class AllowEnv(unittest.TestCase):
    def test_protocol_reservation_allows_the_bump(self):
        self.assertEqual(prepr.allow_env({"protocol": 69, "formats": []}),
                         {"PRE_PR_ALLOW_PROTOCOL": "1"})

    def test_no_reservation_sets_nothing(self):
        self.assertEqual(prepr.allow_env({"protocol": None,
                                          "formats": [26]}), {})
        self.assertEqual(prepr.allow_env(None), {})


class Summary(unittest.TestCase):
    def test_fail_line_printed_first_survives_many_warnings(self):
        out = ("pre-pr: FAIL: kProtocolVersion changed; reserved\n"
               + "\n".join([WARN] * 40))
        s = prepr.summary(out, 800)
        self.assertLessEqual(len(s), 800)
        self.assertTrue(s.startswith("pre-pr: FAIL: kProtocolVersion"))
        self.assertIn("warning", s)   # the tail still fills the rest

    def test_every_fail_line_goes_first(self):
        out = "a\npre-pr: FAIL: one\nb\npre-pr: FAIL: two\nc"
        self.assertEqual(prepr.summary(out, 100),
                         "pre-pr: FAIL: one\npre-pr: FAIL: two\na\nb\nc")

    def test_without_fail_lines_it_is_the_tail(self):
        self.assertEqual(prepr.summary("x" * 50 + "\nend", 5), "x\nend")

    def test_fail_lines_alone_are_cut_to_the_limit(self):
        s = prepr.summary("pre-pr: FAIL: " + "y" * 100, 20)
        self.assertEqual(len(s), 20)


class Recorder:
    def __init__(self, rc=0, out=""):
        self.calls, self.rc, self.out = [], rc, out

    def __call__(self, argv, cwd=None, timeout=120, env=None):
        self.calls.append(env)
        return subprocess.CompletedProcess(argv, self.rc, self.out, "")


class ForgePassesEnv(unittest.TestCase):
    def wt(self):
        wt = tempfile.mkdtemp()
        os.makedirs(os.path.join(wt, "tools"))
        open(os.path.join(wt, "tools", "pre-pr.sh"), "w").close()
        return wt

    def test_env_reaches_the_runner_only_when_given(self):
        rec = Recorder()
        g = forge.GhForge(".", runner=rec)
        g.pre_pr(self.wt())
        g.pre_pr(self.wt(), env={"PRE_PR_ALLOW_PROTOCOL": "1"})
        self.assertEqual(rec.calls, [None, {"PRE_PR_ALLOW_PROTOCOL": "1"}])

    def test_forge_output_keeps_the_fail_line(self):
        out = "pre-pr: FAIL: conflict markers\n" + "\n".join([WARN] * 80)
        ok, text = forge.GhForge(".", runner=Recorder(1, out)).pre_pr(
            self.wt())
        self.assertFalse(ok)
        self.assertTrue(text.startswith("pre-pr: FAIL: conflict markers"))

    def test_real_run_adds_to_the_environment(self):
        r = forge.run([support.sys.executable, "-c",
                       "import os; print(os.environ['PRE_PR_X'], "
                       "'PATH' in os.environ)"], env={"PRE_PR_X": "7"})
        self.assertEqual(r.stdout.split(), ["7", "True"])


def plan(*tasks):
    return {"reserved": {"adr": None, "protocol": None, "formats": []},
            "tasks": [dict({"title": "t", "brief": "b", "files": ["ui/**"]},
                           **t) for t in tasks]}


class FakeStore:
    def __init__(self):
        self.calls = []

    def reserve(self, kind, owner, seed):
        self.calls.append((kind, owner))
        return seed + 1


class PlanNumbers(unittest.TestCase):
    def test_brief_naming_the_constants_needs_both(self):
        p = plan({"brief": "bump kProtocolVersion 68->69 and kFormatVersion"})
        self.assertEqual(plan_numbers.needed(p), ["format", "protocol"])

    def test_files_naming_the_header_need_its_kind(self):
        p = plan({"files": ["contracts/api/include/sonora/api/messages.h"]})
        self.assertEqual(plan_numbers.needed(p), ["protocol"])
        p = plan({"files": ["common/model/include/sonora/model/*_file.h"]})
        self.assertEqual(plan_numbers.needed(p), [])   # not named
        p = plan({"files": ["common/**/session_file.h"]})
        self.assertEqual(plan_numbers.needed(p), ["format"])

    def test_a_directory_glob_or_other_work_needs_nothing(self):
        self.assertEqual(plan_numbers.needed(plan({"files": ["contracts/**"]})),
                         [])
        self.assertEqual(plan_numbers.needed({"tasks": "junk"}), [])
        self.assertEqual(plan_numbers.needed(None), [])

    def test_reserve_fills_empty_slots_and_keeps_given_ones(self):
        st = FakeStore()
        p = plan({"brief": "kProtocolVersion and kFormatVersion"})
        plan_numbers.reserve(st, "feature:f", p, lambda k: 68 if
                             k == "protocol" else 25)
        self.assertEqual(st.calls, [("format", "feature:f"),
                                    ("protocol", "feature:f")])
        self.assertEqual(p["reserved"], {"adr": None, "protocol": 69,
                                         "formats": [26]})
        p = plan({"brief": "kProtocolVersion"})
        p["reserved"]["protocol"] = 5     # validate_plan judges it
        plan_numbers.reserve(FakeStore(), "feature:f", p, lambda k: 68)
        self.assertEqual(p["reserved"]["protocol"], 5)


class DaemonGate(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def feature(self, o, fid):
        o.store.add_task(fid, "daw", "rt-engineer", files=[f"{fid}/**"],
                         feature_id=fid, state="integrating")

    def test_reserved_feature_runs_pre_pr_with_the_allowance(self):
        o = self.make()
        self.feature(o, "side")
        o.store.reserve("protocol", o.flow.owner("side"), 68)
        self.assertTrue(self.pump(o, lambda: self.forge.created))
        self.assertEqual(self.forge.pre_pr_envs,
                         [{"PRE_PR_ALLOW_PROTOCOL": "1"}])

    def test_unreserved_feature_runs_pre_pr_without_it(self):
        o = self.make()
        self.feature(o, "plain")
        o.store.reserve("protocol", o.flow.owner("other"), 68)
        self.assertTrue(self.pump(o, lambda: self.forge.created))
        self.assertEqual(self.forge.pre_pr_envs, [{}])

    def test_fail_line_heads_the_note_and_the_event(self):
        o = self.make()
        self.feature(o, "solo")
        self.forge.pre_pr_result = (
            False, "pre-pr: FAIL: kProtocolVersion changed\n"
            + "\n".join([WARN] * 40))
        self.assertTrue(self.pump(
            o, lambda: o.store.events("solo", "pre_pr_failed")))
        ev = o.store.events("solo", "pre_pr_failed")[0]["detail"]
        self.assertTrue(ev.startswith("pre-pr: FAIL: kProtocolVersion"))
        note = o.store.get_task("solo")["note"]
        self.assertIn("pre-pr: FAIL: kProtocolVersion", note.splitlines()[1])


class PlanTimeReservation(test_feature_flow.FlowCase):
    def test_plan_that_bumps_the_protocol_gets_it_reserved(self):
        o = self.make()
        p = test_feature_flow.canned_plan()
        p["tasks"][0]["brief"] = "add the message; bump kProtocolVersion"
        self.plan(test_feature_flow.FEATURE, p)
        self.script("architect", "architect")
        self.script("lead", "lead")
        self.open(o)
        fid = test_feature_flow.FEATURE
        self.assertTrue(self.pump(
            o, lambda: test_feature_flow.fstate(o) == "building", timeout=30))
        self.assertEqual(o.store.get_feature(fid)["protocol_no"], 11)
        self.assertEqual(o.flow.reserved_for(fid)["protocol"], 11)
        for t in o.store.feature_tasks(fid):
            self.assertEqual(t["protocol_no"], 11)


if __name__ == "__main__":
    unittest.main()

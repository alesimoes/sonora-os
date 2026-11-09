"""routing.toml, tiers, failure routing and the senior-before-principal
round (owner process, 2026-11-06)."""
import os
import shutil
import tempfile
import unittest

import support
from harness import dispatch, failures, launcher, plans, routing, tiers

TOML = '''
[planning]
plan_dir = "planning/plans"
approved_marker = "Status: approved"

[paths]
"common/dsp/**" = "dsp-engineer"
"common/**" = "rt-engineer"
"ui/**" = "ui-engineer"
"tools/harness/**" = "harness-engineer"
".github/**" = "harness-engineer"
"tools/*" = "harness-engineer"

[failures]
product_test_red = "owner-of-path"
flaky_timing_test = "flake-fixer"
sanitizer_blowup = "refactor-engineer"
harness_infra = "harness-engineer"
merge_conflict = "integrator"
stalled_after_hours = 3

[tracks]
daw = ["visual-designer", "rt-engineer", "ui-engineer", "tester"]
cleanup = ["janitor"]

[tiers]
"S/low" = "haiku"
"M/med" = "sonnet"
"L/high" = "opus"
review_min = "sonnet"
'''


def write_repo(root, toml=TOML):
    os.makedirs(os.path.join(root, ".claude", "agents"), exist_ok=True)
    if toml is not None:
        with open(os.path.join(root, ".claude", "routing.toml"), "w") as f:
            f.write(toml)


class RoutingFile(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        write_repo(self.d)
        self.r = routing.load(self.d)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_first_matching_glob_wins(self):
        self.assertEqual(self.r.agent_for_path("common/dsp/a.cpp"),
                         "dsp-engineer")
        self.assertEqual(self.r.agent_for_path("common/model/a.h"),
                         "rt-engineer")
        self.assertEqual(self.r.agent_for_path("tools/harness/x/y.py"),
                         "harness-engineer")
        self.assertEqual(self.r.agent_for_path(".github/workflows/ci.yml"),
                         "harness-engineer")
        self.assertEqual(self.r.agent_for_path("tools/pre-pr.sh"),
                         "harness-engineer")
        self.assertIsNone(self.r.agent_for_path("README.md"))

    def test_star_does_not_cross_directories(self):
        self.assertIsNone(self.r.agent_for_path("tools/other/x.sh"))

    def test_files_pick_the_majority_agent(self):
        self.assertEqual(self.r.agent_for_files(
            ["ui/a.qml", "common/x.h", "ui/b.qml", "docs/a.md"]),
            "ui-engineer")
        self.assertIsNone(self.r.agent_for_files(["docs/a.md"]))

    def test_failure_kinds(self):
        a = self.r.agent_for_failure
        self.assertEqual(a("flaky_timing_test"), "flake-fixer")
        self.assertEqual(a("merge_conflict"), "integrator")
        self.assertEqual(a("product_test_red", ["ui/a.qml"]), "ui-engineer")
        self.assertIsNone(a("unknown_kind"))
        self.assertEqual(self.r.stalled_after_hours(), 3.0)

    def test_tracks_add_specialists_and_keep_old_engineers(self):
        t = routing.track_engineers(self.r)
        self.assertIn("visual-designer", t["daw"])
        self.assertIn("rt-engineer", t["daw"])
        self.assertEqual(set(t), {"daw", "plugin", "os"})   # not cleanup
        self.assertEqual(plans.TRACK_ENGINEERS["plugin"], t["plugin"])

    def test_missing_file_is_the_old_behaviour(self):
        d = tempfile.mkdtemp()
        try:
            self.assertIsNone(routing.load(d))
            self.assertEqual(routing.track_engineers(None),
                             routing.DEFAULT_TRACKS)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_broken_file_is_ignored(self):
        d = tempfile.mkdtemp()
        try:
            write_repo(d, "[paths\nnope")
            self.assertIsNone(routing.load(d))
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_repo_routing_file_parses_when_present(self):
        root = os.path.dirname(support.TOOLS)
        r = routing.load(root)
        if r is not None:
            self.assertTrue(r.paths)


class Tiers(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        write_repo(self.d)
        self.r = routing.load(self.d)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_tier_names_to_models(self):
        m = tiers.task_model
        self.assertEqual(m(None, "junior"), "haiku")
        self.assertEqual(m(None, "pleno"), "sonnet")
        self.assertEqual(m(None, "senior"), "opus")
        self.assertEqual(m(None, "principal"), "fable")

    def test_no_tier_defaults_to_sonnet(self):
        self.assertEqual(tiers.task_model(None), "sonnet")
        self.assertEqual(tiers.task_model(self.r, "nonsense"), "sonnet")

    def test_grade_comes_from_routing_toml(self):
        self.assertEqual(tiers.task_model(self.r, grade="S/low"), "haiku")
        self.assertEqual(tiers.task_model(self.r, grade="L/high"), "opus")
        self.assertEqual(tiers.task_model(None, grade="S/low"), "sonnet")
        # The grade (risk) wins over a tier name picked by size.
        self.assertEqual(tiers.task_model(self.r, "senior", "S/low"), "haiku")
        self.assertEqual(tiers.task_model(self.r, "senior"), "opus")

    def test_reviews_never_run_below_the_floor(self):
        self.assertEqual(tiers.run_model(self.r, "tester", "junior"),
                         "sonnet")
        self.assertEqual(tiers.run_model(self.r, "engineer", "junior"),
                         "haiku")
        self.assertEqual(tiers.run_model(self.r, "tester", "senior"), "opus")

    def test_senior_round_lifts_a_fixer(self):
        self.assertEqual(tiers.run_model(self.r, "fixer", "junior",
                                         senior_round=True), "opus")
        self.assertEqual(tiers.run_model(self.r, "fixer", "pleno"), "sonnet")


class LauncherPicksTheTierModel(unittest.TestCase):
    def argv(self, agent, model):
        return launcher.build_command(["claude"], agent, "p", "m", "s",
                                      model=model)

    def test_model_flag_follows_the_tier(self):
        for tier, want in (("junior", "haiku"), ("pleno", "sonnet"),
                           ("senior", "opus"), ("principal", "fable")):
            a = self.argv("rt-engineer", tiers.task_model(None, tier))
            self.assertEqual(a[a.index("--model") + 1], want)

    def test_principal_engineer_ignores_the_tier(self):
        a = self.argv("principal-engineer", "haiku")
        self.assertEqual(a[a.index("--model") + 1], "fable")


class Classifier(unittest.TestCase):
    def test_kinds(self):
        c = failures.classify
        self.assertEqual(c("ThreadSanitizer: data race"), "sanitizer_finding")
        self.assertEqual(c("tsan job was cancelled: timed out after 50m"),
                         "sanitizer_blowup")
        self.assertEqual(c("docker: error response, build-slot busy"),
                         "harness_infra")
        self.assertEqual(c("test waited, deadline exceeded"),
                         "flaky_timing_test")
        self.assertEqual(c("expected 3 got 4"), "product_test_red")
        self.assertEqual(c(""), "product_test_red")

    def test_agent_for_without_routing(self):
        self.assertIsNone(failures.agent_for(None, "product_test_red"))


class Dispatch(support.DaemonCase):
    def make(self, toml=TOML, agents=(), **kw):
        write_repo(self.tmp, toml)
        for a in agents:
            with open(os.path.join(self.tmp, ".claude", "agents",
                                   a + ".md"), "w") as f:
                f.write(f"---\nname: {a}\n---\n")
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def task(self, o, files, agent="rt-engineer", **kw):
        o.store.add_task("t1", "daw", agent, files=files)
        o.store.set_task("t1", **kw)
        return o.store.get_task("t1")

    def test_fixer_comes_from_the_changed_paths(self):
        o = self.make(agents=["harness-engineer"])
        t = self.task(o, ["tools/harness/x.py"])
        self.assertEqual(dispatch.agent_for(o, t, "fixer"),
                         "harness-engineer")
        self.assertEqual(dispatch.agent_for(o, t, "engineer"),
                         "rt-engineer")      # the lead's assignment stands

    def test_fixer_by_failure_kind(self):
        o = self.make(agents=["flake-fixer", "harness-engineer"])
        t = self.task(o, ["engine/a.cpp"],
                      note="[Engine]\nwait deadline exceeded")
        self.assertEqual(dispatch.agent_for(o, t, "fixer"), "flake-fixer")

    def test_routed_agent_without_a_file_falls_back(self):
        o = self.make(agents=[])
        t = self.task(o, ["tools/harness/x.py"])
        self.assertEqual(dispatch.agent_for(o, t, "fixer"), "rt-engineer")

    def test_without_routing_toml_nothing_changes(self):
        o = self.make(toml=None, agents=["harness-engineer"])
        self.assertIsNone(o.routing)
        t = self.task(o, ["tools/harness/x.py"])
        self.assertEqual(dispatch.agent_for(o, t, "fixer"), "rt-engineer")
        self.assertEqual(dispatch.agent_for(o, t, "principal"),
                         "principal-engineer")
        self.assertEqual(dispatch.model_for(o, t, "fixer"), "sonnet")
        self.assertFalse(dispatch.take_senior_round(o, "f"))

    def test_task_tier_picks_the_model(self):
        o = self.make()
        t = self.task(o, ["engine/a.cpp"])
        self.assertEqual(dispatch.model_for(o, t, "engineer"), "sonnet")
        dispatch.set_tier(o, "t1", "junior")
        self.assertEqual(dispatch.model_for(o, t, "engineer"), "haiku")
        self.assertEqual(dispatch.model_for(o, t, "tester"), "sonnet")
        self.assertIsNone(dispatch.model_for(o, t, "principal"))
        dispatch.set_tier(o, "t1", None, "L/high")
        self.assertEqual(dispatch.model_for(o, t, "engineer"), "opus")

    def test_senior_round_is_taken_once_then_cleared(self):
        o = self.make()
        t = self.task(o, ["engine/a.cpp"], feature_id="f1")
        self.assertTrue(dispatch.take_senior_round(o, "f1"))
        self.assertFalse(dispatch.take_senior_round(o, "f1"))
        self.assertEqual(dispatch.model_for(o, t, "fixer"), "opus")
        dispatch.clear_senior_round(o, "f1")
        self.assertEqual(dispatch.model_for(o, t, "fixer"), "sonnet")
        self.assertTrue(dispatch.take_senior_round(o, "f1"))


if __name__ == "__main__":
    unittest.main()

"""Escalation to the principal engineer, the only agent that runs on Opus
(owner, 2026-11-05): the three triggers, the single slot, the model map and
the fallback to a block."""
import glob
import json
import os
import unittest

import support
from harness import launcher, scheduler

REPO = os.path.dirname(support.TOOLS)


def state(o, tid):
    return o.store.get_task(tid)["state"]


class ModelMap(unittest.TestCase):
    def argv(self, agent):
        return launcher.build_command(["claude"], agent, "p", "m.json",
                                      "s.json")

    def model(self, agent):
        argv = self.argv(agent)
        self.assertEqual(argv.count("--model"), 1)
        return argv[argv.index("--model") + 1]

    def test_principal_engineer_is_the_only_opus(self):
        self.assertEqual(self.model("principal-engineer"), "opus")
        self.assertEqual(launcher.MODEL_OF_AGENT,
                         {"principal-engineer": "opus"})

    def test_every_other_agent_launches_with_sonnet(self):
        names = {os.path.basename(p)[:-3] for p in glob.glob(
            os.path.join(REPO, ".claude", "agents", "*.md"))}
        names |= {"rt-engineer", "tester", "integrator", "lead", "architect",
                  "ui-engineer", "unknown-agent"}
        names.discard("principal-engineer")
        self.assertTrue(names)
        for agent in sorted(names):
            self.assertEqual(self.model(agent), "sonnet", agent)

    def test_agent_file_model_other_than_its_mapped_one_is_refused(self):
        import shutil
        import tempfile
        d = tempfile.mkdtemp()
        try:
            ad = os.path.join(d, ".claude", "agents")
            os.makedirs(ad)

            def write(fn, model):
                with open(os.path.join(ad, fn), "w") as f:
                    f.write(f"---\nname: x\nmodel: {model}\n---\n")
            write("principal-engineer.md", "opus")
            launcher.check_models(d, env={})
            write("principal-engineer.md", "sonnet")
            with self.assertRaises(launcher.ModelError):
                launcher.check_models(d, env={})
            write("principal-engineer.md", "opus")
            write("rt-engineer.md", "opus")
            with self.assertRaises(launcher.ModelError):
                launcher.check_models(d, env={})
        finally:
            shutil.rmtree(d, ignore_errors=True)


class Escalation(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def pr_feature(self, o, fid, pr=100):
        o.store.add_task(fid, "daw", "rt-engineer", files=[f"{fid}/**"],
                         feature_id=fid, state="pr_open")
        o.store.set_feature(fid, pr=pr)

    def back_to_pr_open(self, o, fid):
        for t in o._feature_tasks(fid):
            if t["state"] == "ci_fix":
                o.store.advance(t["id"], "pr_open")
        o.store.set_feature(fid, state="pr_open")

    def red(self, o, fid, jobs=("build",), detail=""):
        o._red(o.store.get_feature(fid), o._feature_tasks(fid), list(jobs),
               "ci", detail)

    # -- triggers ---------------------------------------------------------
    def test_trigger_same_cause_after_two_fix_rounds(self):
        o = self.make()
        self.pr_feature(o, "p")
        for _ in range(2):
            self.red(o, "p", detail="[build]\nboom")
            self.assertFalse(o.store.get_task("p")["escalated"])
            self.back_to_pr_open(o, "p")
        self.red(o, "p", detail="[build]\nboom")
        t = o.store.get_task("p")
        self.assertEqual(t["state"], "ci_fix")
        self.assertIn("same cause still red after 2 fix rounds", t["escalated"])
        ev = o.store.events("p", "escalated")
        self.assertEqual(len(ev), 1)
        self.assertIn("same cause", ev[0]["detail"])

    def test_trigger_configurable_fix_rounds(self):
        o = self.make(principal_after_fixes=1)
        self.pr_feature(o, "p")
        self.red(o, "p")
        self.back_to_pr_open(o, "p")
        self.red(o, "p")
        self.assertTrue(o.store.get_task("p")["escalated"])

    def test_trigger_red_for_more_than_three_hours(self):
        o = self.make()
        self.pr_feature(o, "p")
        self.red(o, "p", ["build"], "[build]\nfirst")
        self.back_to_pr_open(o, "p")
        self.clock.offset += 3 * 3600 + 60
        self.red(o, "p", ["asan"], "[asan]\nother cause")
        t = o.store.get_task("p")
        self.assertIn("more than 3 h", t["escalated"])
        self.assertEqual(o.store.get_feature("p")["fixes"], 2)

    def test_not_escalated_before_three_hours(self):
        o = self.make()
        self.pr_feature(o, "p")
        self.red(o, "p", ["build"])
        self.back_to_pr_open(o, "p")
        self.clock.offset += 3 * 3600 - 600
        self.red(o, "p", ["asan"])
        self.assertFalse(o.store.get_task("p")["escalated"])

    def test_green_ends_the_red_spell(self):
        o = self.make()
        self.pr_feature(o, "p")
        self.forge.prs[100] = {"branch": "feat/p", "files": [],
                               "state": "open", "mergeable": "yes",
                               "checks": {"build": "pass"}}
        self.red(o, "p")
        self.assertTrue(o.store.get_feature("p")["red_since"])
        self.back_to_pr_open(o, "p")
        self.clock.offset += 200
        o.tick()
        self.assertFalse(o.store.get_feature("p")["red_since"])

    def test_trigger_attempts_spent_then_principal_runs_on_opus(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "die")
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        self.assertTrue(self.pump(
            o, lambda: o.store.events("a", "escalated")))
        self.assertEqual(o.store.events("a", "escalated")[0]["detail"],
                         "attempts")
        self.assertNotEqual(state(o, "a"), "blocked")
        self.assertTrue(self.pump(
            o, lambda: any(r["role"] == "principal"
                           for r in o.store.runs_of("a"))))
        self.pump(o, lambda: state(o, "a") == "integrating")
        log = os.path.join(self.fake_dir, "argv.log")
        with open(log) as f:
            argvs = [json.loads(x) for x in f if x.strip()]
        by_agent = {a[a.index("--agent") + 1]: a[a.index("--model") + 1]
                    for a in argvs}
        self.assertEqual(by_agent["principal-engineer"], "opus")
        for agent, model in by_agent.items():
            if agent != "principal-engineer":
                self.assertEqual(model, "sonnet", agent)

    # -- fallback ---------------------------------------------------------
    def test_principal_failing_blocks_and_writes_waits_on_owner(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "die")
        self.script("principal-1", "die")
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "blocked"))
        self.assertEqual(
            [r["role"] for r in o.store.runs_of("a")][-1], "principal")
        with open(os.path.join(o.odir, "waits-on-owner.md")) as f:
            self.assertIn("## a (blocked)", f.read())

    def test_same_cause_after_the_principal_blocks(self):
        o = self.make()
        self.pr_feature(o, "p")
        for _ in range(3):
            self.red(o, "p", detail="[build]\nboom")
            self.back_to_pr_open(o, "p")
        self.red(o, "p", detail="[build]\nboom")
        self.assertEqual(state(o, "p"), "blocked")
        with open(os.path.join(o.odir, "waits-on-owner.md")) as f:
            self.assertIn("principal engineer", f.read())

    # -- single slot ------------------------------------------------------
    def test_one_principal_at_a_time_and_the_other_waits(self):
        o = self.make()
        for fid, pr in (("p", 100), ("q", 101)):
            self.pr_feature(o, fid, pr)
            o.store.advance(fid, "ci_fix")
            o.store.set_task(fid, escalated="x")
        running = [{"task_id": "p", "role": "principal", "name": "principal-1"}]
        plan = scheduler.plan(o.store.tasks(), running, o.cfg, self.clock())
        self.assertEqual(plan.launches, [])
        self.assertEqual(state(o, "q"), "ci_fix")  # waits, not blocked
        plan = scheduler.plan(o.store.tasks(), [], o.cfg, self.clock())
        self.assertEqual([(x.task_id, x.kind) for x in plan.launches[:1]],
                         [("p", "principal")])
        self.assertEqual(sum(x.kind == "principal" for x in plan.launches), 1)
        o.cfg.max_principals = 2
        plan = scheduler.plan(o.store.tasks(), [], o.cfg, self.clock())
        self.assertEqual(sum(x.kind == "principal" for x in plan.launches), 2)

    def test_config_defaults_and_toml(self):
        c = scheduler.Config()
        self.assertEqual((c.principal_after_fixes, c.principal_after_hours,
                          c.max_principals), (2, 3.0, 1))
        p = os.path.join(self.tmp, "o.toml")
        with open(p, "w") as f:
            f.write("principal_after_hours = 5.5\nmax_principals = 2\n")
        c = scheduler.load_config(p)
        if scheduler.tomllib is not None:
            self.assertEqual((c.principal_after_hours, c.max_principals),
                             (5.5, 2))

    # -- brief ------------------------------------------------------------
    def test_brief_carries_trigger_excerpt_and_branch(self):
        o = self.make()
        self.pr_feature(o, "p")
        self.red(o, "p", detail="[build]\nundefined reference to foo")
        for _ in range(2):
            self.back_to_pr_open(o, "p")
            self.red(o, "p", detail="[build]\nundefined reference to foo")
        t = o.store.get_task("p")
        self.assertTrue(t["escalated"])
        extra = o._extra(t, "principal")
        self.assertIn("ESCALATION", extra)
        self.assertIn(t["escalated"], extra)
        self.assertIn("undefined reference to foo", extra)
        self.assertIn(t["branch"], extra)
        self.assertIn("Fixer brief", extra)  # the ordinary brief is kept


if __name__ == "__main__":
    unittest.main()

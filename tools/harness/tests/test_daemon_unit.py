"""Daemon units that need no child process: store, scheduler, launcher."""
import glob
import json
import os
import shutil
import tempfile
import threading
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import launcher, scheduler, status
from harness.store import StateError, Store

NOW = 1_000_000.0


def mkstore(tmp):
    return Store(os.path.join(tmp, "s.db"), clock=lambda: NOW)


def plan(store, runs=(), cfg=None, **kw):
    return scheduler.plan(store.tasks(), list(runs), cfg or scheduler.Config(),
                          NOW, **kw)


def run(task, kind, working=True, building=False, name=None):
    return {"task_id": task, "role": kind, "working": working,
            "building": building, "name": name or f"{kind}-{task}"}


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="orch-u-")
        self.s = mkstore(self.tmp)

    def tearDown(self):
        self.s.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def add(self, n, track="daw", prefix="t", **kw):
        for i in range(n):
            kw.setdefault("files", [f"{prefix}{i}/**"])
            self.s.add_task(f"{prefix}{track}{i}", track, "rt-engineer",
                            **{**kw, "files": [f"{track}/{prefix}{i}/**"]})


class StoreTests(Base):
    def test_state_machine(self):
        self.s.add_task("a", "daw", "rt-engineer")
        with self.assertRaises(StateError):
            self.s.advance("a", "merged")
        for st in ("building", "qa", "bugfix", "qa", "integrating",
                   "pr_open", "ci_fix", "pr_open", "merged"):
            self.s.advance("a", st)
        with self.assertRaises(StateError):
            self.s.advance("a", "building")

    def test_blocked_reachable_from_anywhere_and_resettable(self):
        self.s.add_task("a", "daw", "rt-engineer", state="qa")
        self.s.advance("a", "blocked", note="why")
        self.assertEqual(self.s.get_task("a")["note"], "why")
        self.s.advance("a", "qa")

    def test_advance_resets_attempts_on_change_only(self):
        self.s.add_task("a", "daw", "rt-engineer", state="building")
        self.s.set_task("a", attempts=2)
        self.s.advance("a", "building")
        self.assertEqual(self.s.get_task("a")["attempts"], 2)
        self.s.advance("a", "qa")
        self.assertEqual(self.s.get_task("a")["attempts"], 0)

    def test_reservations_unique_seeded_and_idempotent(self):
        a = self.s.reserve("adr", "x", 47)
        b = self.s.reserve("adr", "y", 47)
        self.assertEqual((a, b), (48, 49))
        self.assertEqual(self.s.reserve("adr", "x", 47), 48)
        self.assertEqual(self.s.reserve("protocol", "x", 50), 51)
        self.assertEqual(self.s.reserve("adr", "z", 60), 61)

    def test_two_writers_one_database(self):
        other = Store(self.s.path)
        errs = []

        def w(store, tag):
            try:
                for i in range(50):
                    store.event(tag, "k", str(i))
                    store.next_counter("c")
            except Exception as e:  # pragma: no cover
                errs.append(e)
        ts = [threading.Thread(target=w, args=(x, t))
              for x, t in ((self.s, "a"), (other, "b"))]
        [t.start() for t in ts]
        [t.join() for t in ts]
        other.close()
        self.assertEqual(errs, [])
        self.assertEqual(self.s.get_meta("c"), "100")


class SchedulerTests(Base):
    def test_floors_filled_and_cap_respected(self):
        self.add(40)
        p = plan(self.s, cfg=scheduler.Config(cap=20))
        self.assertEqual(len(p.launches), 20)
        p = plan(self.s, cfg=scheduler.Config(cap=5))
        self.assertEqual(len(p.launches), 5)

    def test_cap_never_above_20_or_env(self):
        self.assertEqual(scheduler.Config(cap=99).cap, 20)
        self.assertEqual(scheduler.max_agents_cap(
            {"CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS": "7"}), 7)
        self.assertEqual(scheduler.max_agents_cap(
            {"CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS": "50"}), 20)

    def test_live_runs_use_seats(self):
        self.add(10)
        runs = [run(f"x{i}", "tester") for i in range(18)]
        p = plan(self.s, runs, scheduler.Config(cap=20))
        self.assertEqual(len(p.launches), 2)

    def test_tracks_share_the_floor_round_robin(self):
        self.add(20, "daw")
        self.add(10, "os")
        cfg = scheduler.Config(cap=6, floors={"daw": 8, "os": 4})
        p = plan(self.s, cfg=cfg)
        tracks = [self.s.get_task(l.task_id)["track"] for l in p.launches]
        self.assertEqual(tracks.count("os"), 3)
        self.assertEqual(tracks.count("daw"), 3)

    def test_priority_fix_tester_integrator_engineer_planner_burst(self):
        self.s.add_task("e1", "daw", "rt-engineer", files=["a/**"])
        self.s.add_task("e2", "daw", "rt-engineer", files=["b/**"])
        self.s.add_task("q", "daw", "rt-engineer", state="qa")
        self.s.add_task("i", "daw", "rt-engineer", state="integrating")
        self.s.add_task("f", "daw", "rt-engineer", state="ci_fix")
        self.s.add_task("w", None, "lead", kind="wake")
        cfg = scheduler.Config(cap=20, floors={"daw": 1})
        p = plan(self.s, cfg=cfg)
        order = [(l.task_id, l.priority) for l in p.launches]
        self.assertEqual([t for t, _ in order],
                         ["f", "q", "i", "e1", "w", "e2"])
        self.assertEqual([pr for _, pr in order],
                         [scheduler.P_FIX, scheduler.P_TESTER,
                          scheduler.P_INTEGRATOR, scheduler.P_ENGINEER,
                          scheduler.P_PLANNER, scheduler.P_BURST])

    def test_top_priorities_win_the_last_seats(self):
        self.add(5)
        self.s.add_task("f", "daw", "rt-engineer", state="ci_fix")
        self.s.add_task("q", "daw", "rt-engineer", state="qa")
        p = plan(self.s, cfg=scheduler.Config(cap=2))
        self.assertEqual([l.task_id for l in p.launches], ["f", "q"])

    def test_building_is_not_working(self):
        # 3 live engineers, all idle: the floor of 3 is not met, so the
        # daemon launches more instead of counting them.
        self.add(8)
        busy = [run(f"b{i}", "engineer", working=False) for i in range(3)]
        for i in range(3):
            self.s.add_task(f"b{i}", "daw", "rt-engineer", state="building")
        cfg = scheduler.Config(cap=20, floors={"daw": 3})
        p = plan(self.s, busy, cfg)
        n = len([l for l in p.launches if l.priority == scheduler.P_ENGINEER])
        self.assertEqual(n, 3)
        busy_ok = [run(f"b{i}", "engineer", working=True) for i in range(3)]
        p = plan(self.s, busy_ok, cfg)
        self.assertFalse([l for l in p.launches
                          if l.priority == scheduler.P_ENGINEER])

    def test_no_overlapping_files(self):
        self.s.add_task("a", "daw", "rt-engineer", files=["engine/**"])
        self.s.add_task("b", "daw", "rt-engineer", files=["engine/x/*.cpp"])
        self.s.add_task("c", "daw", "rt-engineer", files=["ui/**"])
        p = plan(self.s)
        self.assertEqual([l.task_id for l in p.launches], ["a", "c"])
        self.s.advance("a", "building")
        p = plan(self.s, [run("a", "engineer")])
        self.assertEqual([l.task_id for l in p.launches], ["c"])

    def test_dependencies_must_be_qa_passed(self):
        self.s.add_task("a", "daw", "rt-engineer", files=["a/**"])
        self.s.add_task("b", "daw", "rt-engineer", files=["b/**"],
                        depends_on=["a"])
        self.assertEqual([l.task_id for l in plan(self.s).launches], ["a"])
        self.s.advance("a", "building")
        self.s.advance("a", "qa")
        self.s.advance("a", "integrating")
        ids = [l.task_id for l in plan(self.s).launches]
        self.assertIn("b", ids)

    def test_kill_switch_and_usage_pause_launch_nothing(self):
        self.add(5)
        p = plan(self.s, stop=True)
        self.assertEqual(p.launches, [])
        self.assertEqual(p.gaps["daw"], "kill switch")
        p = plan(self.s, usage_pause_until=NOW + 60)
        self.assertEqual((p.launches, p.gaps["daw"]), ([], "usage pause"))

    def test_pause_pool_and_track(self):
        self.s.add_task("q", "daw", "rt-engineer", state="qa")
        self.add(2)
        p = plan(self.s, paused={"qa"})
        self.assertNotIn("q", [l.task_id for l in p.launches])
        p = plan(self.s, paused={"daw"})
        self.assertEqual(p.launches, [])

    def test_open_pr_budget_holds_integrators_and_ready_unpr_holds_builders(self):
        self.s.add_task("i", "daw", "rt-engineer", state="integrating")
        for n in range(10):
            self.s.add_task(f"p{n}", "daw", "rt-engineer", state="pr_open")
        p = plan(self.s)
        self.assertEqual(p.launches, [])
        cfg = scheduler.Config(max_ready_unpr=1)
        self.add(3)
        p = plan(self.s, cfg=cfg)
        self.assertFalse([l for l in p.launches if l.kind == "engineer"])
        self.assertEqual(p.gaps["daw"], "CI budget")

    def test_local_build_limit_gates_testers(self):
        for i in range(3):
            self.s.add_task(f"q{i}", "daw", "rt-engineer", state="qa")
        cfg = scheduler.Config(local_builds=1)
        b = [run("x", "engineer", building=True)]
        self.assertEqual(plan(self.s, b, cfg).launches, [])
        self.assertEqual(len(plan(self.s, [], cfg).launches), 3)
        self.assertGreaterEqual(scheduler.default_local_builds(1), 1)
        self.assertEqual(scheduler.default_local_builds(16), 4)

    def test_under_floor_without_eligible_task_wakes_the_lead(self):
        p = plan(self.s)
        self.assertIn(("lead", "pool_under_floor", "daw"), p.wakes)
        self.assertEqual(p.gaps["daw"], "no eligible task")

    def test_backoff_holds_a_task(self):
        self.add(1)
        self.s.set_task("tdaw0", not_before=NOW + 60)
        self.assertEqual(plan(self.s).launches, [])


class LauncherTests(unittest.TestCase):
    def cmd(self, agent="rt-engineer", **kw):
        return launcher.build_command(["claude"], agent, "do it", "m.json",
                                      "s.json", **kw)

    def test_sonnet_on_every_argv_exactly_once(self):
        for agent in ("rt-engineer", "tester", "integrator", "lead",
                      "architect", "ui-engineer"):
            argv = self.cmd(agent)
            idx = [i for i, a in enumerate(argv) if a == "--model"]
            self.assertEqual(len(idx), 1)
            self.assertEqual(argv[idx[0] + 1], "sonnet")
            self.assertEqual(argv[:2], ["claude", "-p"])
            self.assertIn("--strict-mcp-config", argv)
            self.assertNotIn("opus", " ".join(argv).lower())

    def test_no_parameter_or_environment_can_change_the_model(self):
        import inspect
        self.assertNotIn("model", inspect.signature(
            launcher.build_command).parameters)
        os.environ["CLAUDE_MODEL"] = "opus"
        try:
            self.assertEqual(self.cmd()[2:4], ["--model", "sonnet"])
            with self.assertRaises(launcher.ModelError):
                launcher.check_models(tempfile.gettempdir())
            self.assertNotIn("CLAUDE_MODEL", self._spawn_env())
        finally:
            del os.environ["CLAUDE_MODEL"]

    def _spawn_env(self):
        import subprocess
        import sys
        d = tempfile.mkdtemp()
        try:
            p = launcher.spawn([sys.executable, "-c",
                                "import os,json;print(json.dumps(dict("
                                "os.environ)))"], {}, d,
                               os.path.join(d, "out"))
            p.wait()
            with open(os.path.join(d, "out")) as f:
                return json.loads(f.read())
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_agent_file_with_other_model_refuses_to_start(self):
        d = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(d, ".claude", "agents"))
            p = os.path.join(d, ".claude", "agents", "x.md")
            with open(p, "w") as f:
                f.write("---\nname: x\nmodel: opus\n---\nbody\n")
            with self.assertRaises(launcher.ModelError):
                launcher.check_models(d, env={})
            with open(p, "w") as f:
                f.write("---\nname: x\nmodel: sonnet\n---\nbody\n")
            launcher.check_models(d, env={})
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_repo_agents_are_all_sonnet(self):
        root = os.path.dirname(support.TOOLS)
        launcher.check_models(root, env={})

    def test_model_flag_exists_only_in_launcher(self):
        hits = []
        for f in glob.glob(os.path.join(support.TOOLS, "harness", "*.py")) \
                + [os.path.join(support.TOOLS, "orchestrator.py")]:
            with open(f, encoding="utf-8") as fh:
                text = fh.read()
            if '"--model"' in text or "'--model'" in text:
                hits.append(os.path.basename(f))
        self.assertEqual(hits, ["launcher.py"])

    def test_fake_claude_swap_and_run_files(self):
        self.assertEqual(launcher.claude_argv(None), ["claude"])
        self.assertEqual(launcher.claude_argv("f.py")[-1], "f.py")
        d = tempfile.mkdtemp()
        try:
            env = launcher.run_env("eng-1", "t", "f", d, 99, "tok", "r.json")
            mcp, settings = launcher.write_run_files(
                os.path.join(d, "run"), env)
            with open(mcp) as f:
                cfg = json.load(f)
            srv = cfg["mcpServers"]["harness"]
            self.assertTrue(srv["args"][0].endswith("harness_mcp.py"))
            self.assertEqual(srv["env"]["SONORA_AGENT_NAME"], "eng-1")
            self.assertEqual(srv["env"]["SONORA_HARNESS_PORT"], "99")
            with open(settings) as f:
                hooks = json.load(f)["hooks"]
            self.assertEqual(set(hooks), {"PostToolUse", "UserPromptSubmit",
                                          "PreToolUse"})
        finally:
            shutil.rmtree(d, ignore_errors=True)


class StatusTests(Base):
    def test_atomic_write_keeps_old_file_if_replace_fails(self):
        p = os.path.join(self.tmp, "status.json")
        status.write_atomic(p, {"v": 1})
        real = os.replace
        os.replace = lambda *a: (_ for _ in ()).throw(OSError("boom"))
        try:
            with self.assertRaises(OSError):
                status.write_atomic(p, {"v": 2})
        finally:
            os.replace = real
        with open(p) as f:
            self.assertEqual(json.load(f), {"v": 1})
        status.write_atomic(p, {"v": 3})
        self.assertEqual(sorted(x for x in os.listdir(self.tmp)
                                if x.startswith("status")), ["status.json"])


if __name__ == "__main__":
    unittest.main()

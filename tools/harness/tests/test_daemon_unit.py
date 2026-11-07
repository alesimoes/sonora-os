"""Daemon units that need no child process: store, scheduler, launcher."""
import glob
import json
import os
import shutil
import sqlite3
import tempfile
import threading
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
from harness import launcher, scheduler, status
from harness.store import StateError, Store

NOW = 1_000_000.0


def mkstore(tmp):
    return Store(os.path.join(tmp, "s.json"), clock=lambda: NOW)


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

    def test_threads_share_one_store(self):
        errs = []

        def w(tag):
            try:
                for i in range(30):
                    self.s.event(tag, "k", str(i))
                    self.s.next_counter("c")
            except Exception as e:  # pragma: no cover
                errs.append(e)
        ts = [threading.Thread(target=w, args=(t,)) for t in ("a", "b")]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(errs, [])
        self.assertEqual(self.s.get_meta("c"), "60")
        self.assertEqual(len(self.s.events(limit=1000)), 60)

    def test_unique_feature_pr(self):
        self.s.add_feature("f1", "daw")
        self.s.add_feature("f2", "daw")
        self.s.set_feature("f1", pr=7)
        self.s.set_feature("f1", pr=7)
        with self.assertRaises(StateError):
            self.s.set_feature("f2", pr=7)
        self.assertIsNone(self.s.get_feature("f2")["pr"])

    def test_tx_rolls_back_on_error(self):
        self.s.add_task("a", "daw", "rt-engineer")
        with self.assertRaises(RuntimeError):
            with self.s.tx():
                self.s.set_task("a", note="x")
                raise RuntimeError
        self.assertEqual(self.s.get_task("a")["note"], "")
        self.assertEqual(mkstore(self.tmp).get_task("a")["note"], "")

    def test_state_survives_reopen(self):
        self.s.add_task("a", "daw", "rt-engineer", files=["x/**"])
        self.s.add_run(id="r1", task_id="a", role="engineer", name="n",
                       started=NOW)
        self.s.reserve("adr", "a", 10)
        self.s.set_meta("k", 5)
        t = mkstore(self.tmp)
        self.assertEqual(t.get_task("a")["files"], ["x/**"])
        self.assertEqual([r["id"] for r in t.active_runs()], ["r1"])
        self.assertEqual(t.reserve("adr", "a", 10), 11)
        self.assertEqual(t.get_meta("k"), "5")
        self.assertIsNotNone(t.get_feature("a"))

    def test_heartbeat_is_saved_by_flush(self):
        self.s.add_task("a", "daw", "rt-engineer")
        self.s.add_run(id="r1", task_id="a", role="engineer", name="n",
                       started=NOW, last_beat=NOW)
        self.s.set_run("r1", last_beat=NOW + 5)
        self.assertEqual(mkstore(self.tmp).get_run("r1")["last_beat"], NOW)
        self.s.flush()
        self.assertEqual(mkstore(self.tmp).get_run("r1")["last_beat"],
                         NOW + 5)


class StorePersistenceTests(Base):
    def state(self):
        with open(self.s.path, encoding="utf-8") as f:
            return json.load(f)

    def test_save_failure_keeps_old_file_and_retries(self):
        self.s.add_task("a", "daw", "rt-engineer")
        before = self.state()
        with mock.patch("harness.store.os.replace",
                        side_effect=OSError("disk gone")):
            with self.assertRaises(OSError):
                self.s.set_task("a", note="new")
        self.assertEqual(self.state(), before)
        self.assertEqual([n for n in os.listdir(self.tmp)
                          if n.endswith(".tmp")], [])
        self.assertEqual(mkstore(self.tmp).get_task("a")["note"], "")
        self.s.flush()   # dirty stayed set; the retry lands
        self.assertEqual(mkstore(self.tmp).get_task("a")["note"], "new")

    def test_save_failure_inside_tx_commit(self):
        self.s.add_task("a", "daw", "rt-engineer")
        with mock.patch("harness.store.os.replace",
                        side_effect=OSError("disk gone")):
            with self.assertRaises(OSError):
                self.s.advance("a", "building")
        self.assertEqual(mkstore(self.tmp).get_task("a")["state"], "queued")

    def test_future_schema_refused(self):
        with open(self.s.path, "w") as f:
            json.dump({"schema": 99}, f)
        with self.assertRaises(StateError):
            mkstore(self.tmp)

    def test_run_whose_task_is_missing_is_quarantined(self):
        self.s.add_task("a", "daw", "rt-engineer")
        self.s.add_run(id="r1", task_id="a", role="engineer", name="n",
                       started=NOW)
        self.s.add_run(id="r2", task_id="a", role="engineer", name="m",
                       started=NOW)
        doc = self.state()
        del doc["tasks"]["a"]
        with open(self.s.path, "w") as f:
            json.dump(doc, f)
        t = mkstore(self.tmp)   # must not raise
        self.assertEqual(t.active_runs(), [])
        self.assertIsNone(t.get_task("a"))
        with open(os.path.join(self.tmp, "s.quarantine.json")) as f:
            q = json.load(f)
        self.assertEqual({(x["table"], x["key"]) for x in q},
                         {("runs", "r1"), ("runs", "r2")})
        self.assertTrue(t.events(kind="quarantined"))
        self.assertEqual(self.state()["runs"], {})   # cleaned file saved

    def test_other_violations_are_quarantined(self):
        self.s.add_task("a", "daw", "rt-engineer")
        self.s.add_task("b", "daw", "rt-engineer")
        self.s.add_task("c", "daw", "rt-engineer")
        doc = self.state()
        doc["tasks"]["a"]["state"] = "bogus"
        doc["tasks"]["b"]["feature_id"] = "nope"
        doc["features"]["a"]["pr"] = 3
        doc["features"]["b"]["pr"] = 3
        doc["reservations"] = [{"kind": "adr", "number": 5, "task_id": "c"},
                               {"kind": "adr", "number": 5, "task_id": "d"},
                               {"kind": "zzz", "number": 1}]
        with open(self.s.path, "w") as f:
            json.dump(doc, f)
        t = mkstore(self.tmp)
        self.assertEqual([x["id"] for x in t.tasks()], ["c"])
        self.assertEqual(len(t.reservations()), 1)
        with open(os.path.join(self.tmp, "s.quarantine.json")) as f:
            self.assertGreaterEqual(len(json.load(f)), 4)

    def test_migration_from_sqlite(self):
        db = os.path.join(self.tmp, "old.db")
        c = sqlite3.connect(db)
        c.executescript("""
        CREATE TABLE features(id TEXT PRIMARY KEY, plan_path TEXT, branch TEXT,
          track TEXT, state TEXT DEFAULT 'planning', pr INTEGER, adr_no INTEGER,
          protocol_no INTEGER, partial INTEGER DEFAULT 0, created REAL,
          updated REAL);
        CREATE TABLE tasks(id TEXT PRIMARY KEY, kind TEXT DEFAULT 'work',
          backlog_id TEXT, feature_id TEXT, track TEXT, agent TEXT, state TEXT,
          branch TEXT, worktree TEXT, pr INTEGER, adr_no INTEGER,
          protocol_no INTEGER, attempts INTEGER DEFAULT 0,
          bounces INTEGER DEFAULT 0, depends_on TEXT DEFAULT '[]',
          files TEXT DEFAULT '[]', brief TEXT DEFAULT '', eng_name TEXT,
          cur_name TEXT, wants TEXT DEFAULT '', not_before REAL DEFAULT 0,
          priority INTEGER DEFAULT 0, created REAL, updated REAL,
          note TEXT DEFAULT '');
        CREATE TABLE runs(id TEXT PRIMARY KEY, task_id TEXT, role TEXT,
          name TEXT, state TEXT, pid INTEGER, started REAL, last_beat REAL,
          ended REAL, exit INTEGER, result_path TEXT, stream_path TEXT,
          worktree TEXT, session_id TEXT, suspect INTEGER DEFAULT 0,
          outcome TEXT);
        CREATE TABLE events(ts REAL, task_id TEXT, kind TEXT, detail TEXT);
        CREATE TABLE reservations(kind TEXT, number INTEGER, task_id TEXT,
          PRIMARY KEY(kind, number));
        CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        INSERT INTO features(id, track, pr, created) VALUES('f','daw',9,1);
        INSERT INTO tasks(id, feature_id, track, state, depends_on, files,
          created) VALUES('t','f','daw','qa','["x"]','["a/**"]',1);
        INSERT INTO tasks(id, feature_id, track, state, created)
          VALUES('orphan','gone','daw','qa',2);
        INSERT INTO runs(id, task_id, started) VALUES('r','t',5);
        INSERT INTO reservations VALUES('adr', 60, 't');
        INSERT INTO meta VALUES('run','12');
        INSERT INTO events VALUES(1.0, 't', 'old', 'ev');
        """)
        c.commit()
        c.close()
        t = Store(os.path.join(self.tmp, "old.json"), clock=lambda: NOW)
        self.assertEqual(t.get_task("t")["depends_on"], ["x"])
        self.assertEqual(t.get_task("t")["files"], ["a/**"])
        self.assertEqual(t.get_feature("f")["pr"], 9)
        self.assertEqual(t.get_task("t")["attempts"], 0)
        self.assertEqual(t.next_counter("run"), 13)
        self.assertEqual(t.reserve("adr", "new", 10), 61)
        self.assertEqual(len(t.runs_of("t")), 1)
        self.assertIsNone(t.get_task("orphan"))   # quarantined
        self.assertTrue(os.path.exists(db + ".migrated"))
        self.assertFalse(os.path.exists(db))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "old.json")))
        self.assertTrue(t.events(kind="old"))
        # a second open reads the JSON and does not migrate again
        self.assertEqual(Store(t.path).get_task("t")["state"], "qa")

    def test_event_rotation(self):
        with mock.patch("harness.store.EVENT_ROTATE_BYTES", 400):
            for i in range(40):
                self.s.event("a", "k", f"event number {i}")
        names = sorted(n for n in os.listdir(self.tmp)
                       if n.startswith("events"))
        self.assertGreater(len(names), 2)
        self.assertIn("events.1.jsonl", names)
        evs = self.s.events(limit=1000)
        self.assertEqual([e["detail"] for e in evs],
                         [f"event number {i}" for i in range(39, -1, -1)])
        self.assertEqual(len(self.s.events(limit=5)), 5)
        self.assertEqual(self.s.events(limit=1)[0]["detail"],
                         "event number 39")

    def test_torn_last_event_line_is_skipped(self):
        self.s.event("a", "k", "one")
        with open(self.s.events_path, "a") as f:
            f.write('{"ts": 1, "task')
        self.assertEqual([e["detail"] for e in self.s.events()], ["one"])


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

    def test_ready_budget_counts_features_and_never_starves_a_sibling(self):
        # Two slices of one feature are one ready feature; the third slice
        # (an engineer) must still launch although the budget is full.
        cfg = scheduler.Config(max_ready_unpr=2)
        self.s.add_task("a1", "daw", "rt-engineer", state="integrating",
                        feature_id="fa")
        self.s.add_task("a2", "daw", "rt-engineer", state="integrating",
                        feature_id="fa")
        self.s.add_task("b1", "daw", "rt-engineer", state="integrating",
                        feature_id="fb")
        self.s.add_task("a3", "daw", "rt-engineer", feature_id="fa")
        self.s.add_task("c1", "daw", "rt-engineer", feature_id="fc")
        p = plan(self.s, cfg=cfg)
        launched = [l.task_id for l in p.launches if l.kind == "engineer"]
        self.assertIn("a3", launched)
        self.assertNotIn("c1", launched)

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

    def test_only_the_four_tier_models_can_be_asked_for(self):
        for m in ("haiku", "sonnet", "opus", "fable"):
            self.assertEqual(self.cmd(model=m)[2:4], ["--model", m])
        with self.assertRaises(launcher.ModelError):
            self.cmd(model="gpt-4")
        # the environment never selects the model, and a foreign one stops
        # the daemon
        os.environ["CLAUDE_MODEL"] = "gpt-4"
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
                f.write("---\nname: x\nmodel: gpt-4\n---\nbody\n")
            with self.assertRaises(launcher.ModelError):
                launcher.check_models(d, env={})
            for m in ("haiku", "sonnet", "opus", "fable"):
                with open(p, "w") as f:
                    f.write(f"---\nname: x\nmodel: {m}\n---\nbody\n")
                launcher.check_models(d, env={})
            launcher.check_models(d, env={"ANTHROPIC_MODEL": "fable"})
            with self.assertRaises(launcher.ModelError):
                launcher.check_models(d, env={"ANTHROPIC_MODEL": "gpt-4"})
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_settings_json_model_must_be_sonnet(self):
        d = tempfile.mkdtemp()
        try:
            os.makedirs(os.path.join(d, ".claude"))
            p = os.path.join(d, ".claude", "settings.json")
            for body in ({"model": "gpt-4"},
                         {"env": {"ANTHROPIC_MODEL": "gemini-2"}}):
                with open(p, "w") as f:
                    json.dump(body, f)
                with self.assertRaises(launcher.ModelError):
                    launcher.check_models(d, env={})
            with open(p, "w") as f:
                json.dump({"model": "opus", "env": {"X": "1"}}, f)
            launcher.check_models(d, env={})
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_token_not_on_disk_and_stream_redacted(self):
        import sys
        d = tempfile.mkdtemp()
        try:
            env = launcher.run_env("eng-1", "t", "f", d, 99, "sekrit-tok",
                                   "r.json")
            run = os.path.join(d, "run")
            launcher.write_run_files(run, env)
            stream = os.path.join(run, "stream.log")
            p = launcher.spawn(
                [sys.executable, "-c",
                 "import os;print('tok=' + os.environ['SONORA_HARNESS_TOKEN']"
                 " + os.environ['SONORA_TOKEN'])"], env, d, stream)
            p.wait()
            p.pump.join(5)
            for root, _, files in os.walk(d):
                for fn in files:
                    with open(os.path.join(root, fn), "rb") as f:
                        self.assertNotIn(b"sekrit-tok", f.read(), fn)
            with open(stream, "rb") as f:
                self.assertIn(b"tok=******", f.read())
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

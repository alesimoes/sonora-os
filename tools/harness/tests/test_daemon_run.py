"""Daemon against the fake claude: real child processes, no quota."""
import json
import os
import subprocess
import sys
import time
import unittest

import support
from harness import heartbeat, launcher


def state(o, tid):
    return o.store.get_task(tid)["state"]


class RunTests(support.DaemonCase):
    def add(self, o, tid, track="daw", **kw):
        o.store.add_task(tid, track, kw.pop("agent", "rt-engineer"),
                         files=[f"{tid}/**"], **kw)

    def test_happy_path_building_qa_integrating(self):
        o = self.make(floors={"daw": 1})
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "integrating"))
        evs = [e["detail"] for e in o.store.events("a", "result")]
        self.assertEqual(sorted(evs), ["engineer done", "tester done"])
        sent = [(t, ty) for t, ty, _ in o.broker.sent]
        self.assertIn(("lead", "task_done"), sent)
        names = [r["name"] for r in o.store.runs_of("a")]
        self.assertEqual(names[0][:4], "eng-")
        self.assertEqual(names[1][:7], "tester-")
        self.assertTrue(o.store.get_task("a")["worktree"])

    def test_every_launch_argv_is_sonnet(self):
        o = self.make(floors={"daw": 1})
        self.add(o, "a")
        self.pump(o, lambda: state(o, "a") == "integrating")
        log = os.path.join(self.fake_dir, "argv.log")

        def read():
            # the woken lead is launched on the last tick; its fake process
            # logs argv a moment later
            try:
                with open(log) as f:
                    return [json.loads(x) for x in f if x.strip()]
            except (OSError, ValueError):
                return []
        self.pump(o, lambda: len(read()) >= 3)
        argvs = read()
        # engineer, tester and the lead woken by the empty pool
        self.assertEqual(len(argvs), 3)
        for a in argvs:
            self.assertEqual(a[a.index("--model") + 1], "sonnet")
            self.assertEqual(a.count("--model"), 1)

    def test_dry_run_launches_nothing(self):
        o = self.make(dry_run=True, floors={"daw": 2})
        self.add(o, "a")
        self.add(o, "b")
        for _ in range(3):
            o.tick()
        self.assertEqual(o.store.active_runs(), [])
        self.assertEqual(state(o, "a"), "queued")
        self.assertFalse(os.path.exists(os.path.join(self.fake_dir,
                                                     "argv.log")))
        self.assertEqual(len(o.store.events("a", "dry_run_launch")), 1)
        with open(os.path.join(self.tmp, "planning", "orchestrator",
                               "status.json")) as f:
            self.assertEqual(json.load(f)["mode"], "dry-run")

    def test_live_pools_keep_other_pools_dry(self):
        o = self.make(floors={"daw": 1}, live_pools=["qa"])
        self.add(o, "a")
        o.tick()
        self.assertEqual(o.store.active_runs(), [])

    def test_floors_filled_and_cap_respected_with_processes(self):
        self.script("eng-1", "hang")
        os.environ["FAKE_CLAUDE_DEFAULT"] = "hang"
        o = self.make(cap=5, floors={"daw": 3, "os": 2})
        for i in range(6):
            self.add(o, f"d{i}")
        for i in range(4):
            self.add(o, f"o{i}", track="os")
        o.tick()
        o.tick()
        runs = o.store.active_runs()
        self.assertEqual(len(runs), 5)
        tracks = [o.store.get_task(r["task_id"])["track"] for r in runs]
        self.assertEqual((tracks.count("daw"), tracks.count("os")), (3, 2))
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        self.assertEqual(st["pools"]["build:daw"]["working"], 3)
        self.assertEqual(st["live"], 5)

    def test_dead_run_relaunched_with_same_name_then_blocked(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "die")
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "blocked"))
        runs = o.store.runs_of("a")
        self.assertEqual(len(runs), 4)  # first launch + 3 relaunches
        self.assertEqual({r["name"] for r in runs}, {"eng-1"})
        self.assertIn("relaunches spent", o.store.get_task("a")["note"])
        with open(os.path.join(o.odir, "waits-on-owner.md")) as f:
            self.assertIn("## a (blocked)", f.read())
        self.assertIn("agent_dead", [ty for _, ty, _ in o.broker.sent])
        with open(os.path.join(o.odir, "status.json")) as f:
            self.assertEqual(json.load(f)["waits_on_owner"][0]["task"], "a")

    def test_dead_then_recovers(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "die", "done")
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "integrating"))
        self.assertEqual(len(o.store.runs_of("a")), 4)

    def test_backoff_delays_the_relaunch(self):
        o = self.make(floors={"daw": 1})
        o.cfg.backoff_s = (60, 300, 900)
        self.script("eng-1", "die", "done")
        self.add(o, "a")
        self.pump(o, lambda: o.store.get_task("a")["attempts"] == 1)
        for _ in range(3):
            o.tick()
        self.assertEqual(len(o.store.runs_of("a")), 1)
        self.clock.offset += 61
        self.assertTrue(self.pump(o, lambda: len(o.store.runs_of("a")) >= 2))

    def test_invalid_result_is_a_dead_run(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "invalid", "done")
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "integrating"))
        self.assertEqual(o.store.runs_of("a")[0]["outcome"], "dead")

    def test_hung_process_is_killed_and_relaunched(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "hang", "done")
        self.add(o, "a")
        self.pump(o, lambda: len(o.store.active_runs()) == 1)
        first = o.store.active_runs()[0]
        # The fake consumes its script line on startup; killing it before
        # that would hand the relaunch the "hang" line again.
        self.assertTrue(self.pump(
            o, lambda: os.path.getsize(first["stream_path"]) > 0))
        self.clock.offset += o.cfg.suspect_s + 60
        o.tick()
        self.assertEqual(o.store.get_run(first["id"])["suspect"], 1)
        self.clock.offset += o.cfg.stalled_s
        self.assertTrue(self.pump(
            o, lambda: o.store.get_run(first["id"])["outcome"] == "stalled"))
        # The stream log mtime is real time; with the skew kept the relaunched
        # run would itself look stalled before the fake gets to write.
        self.clock.offset = 0
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "integrating"))
        self.assertEqual(o.store.get_run(first["id"])["outcome"], "stalled")
        self.assertFalse(launcher.pid_alive(first["pid"]))

    def test_ceiling_kills_even_a_beating_run(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "hang", "done")
        self.add(o, "a")
        self.pump(o, lambda: len(o.store.active_runs()) == 1)
        first = o.store.active_runs()[0]
        o.cfg.stalled_s = 10 ** 9
        self.clock.offset += o.cfg.ceiling_s + 1
        o.tick()
        self.assertEqual(o.store.get_run(first["id"])["outcome"], "ceiling")

    def test_tester_bug_bounces_to_the_same_engineer_then_blocks(self):
        o = self.make(floors={"daw": 1})
        # tester-1 finds a bug twice, third bounce blocks the task.
        os.environ["FAKE_CLAUDE_DEFAULT"] = "done"
        for n in (1, 2, 3):
            self.script(f"tester-{n}", "bug")
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "blocked"))
        t = o.store.get_task("a")
        self.assertEqual(t["bounces"], 3)
        self.assertIn("third bounce", t["note"])
        eng = {r["name"] for r in o.store.runs_of("a")
               if r["role"] == "engineer"}
        self.assertEqual(eng, {"eng-1"})
        # the second bounce woke the architect
        wakes = [w["id"] for w in o.store.tasks(kind="wake")]
        self.assertTrue(any("architect-bug_ruling" in w for w in wakes))
        types = [ty for _, ty, _ in o.broker.sent]
        self.assertEqual(types.count("qa_fail"), 2)

    def test_agent_blocked_result_blocks_the_task(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "blocked")
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "blocked"))

    def test_usage_limit_pauses_without_counting(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "limit", "done")
        self.add(o, "a")
        self.assertTrue(self.pump(
            o, lambda: o.usage_pause_until > 0 and not o.store.active_runs()))
        t = o.store.get_task("a")
        self.assertEqual(t["attempts"], 0)
        n = len(o.store.runs_of("a"))
        for _ in range(3):
            o.tick()
        self.assertEqual(len(o.store.runs_of("a")), n)  # paused
        with open(os.path.join(o.odir, "status.json")) as f:
            self.assertTrue(json.load(f)["usage_pause_until"])
        self.clock.offset += 601
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "integrating"))

    def test_stop_drains_and_stop_now_kills(self):
        o = self.make(floors={"daw": 2})
        os.environ["FAKE_CLAUDE_DEFAULT"] = "hang"
        self.add(o, "a")
        self.add(o, "b")
        self.add(o, "c")
        o.tick()
        self.assertEqual(len(o.store.active_runs()), 3)  # floor 2 + burst
        stop = os.path.join(o.odir, "STOP")
        open(stop, "w").close()
        o.store.add_task("d", "daw", "rt-engineer")
        for _ in range(3):
            o.tick()
        self.assertEqual(len(o.store.active_runs()), 3)  # drain, no new
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        self.assertEqual(st["kill_switch"], "STOP")
        pids = [r["pid"] for r in o.store.active_runs()]
        open(os.path.join(o.odir, "STOP.NOW"), "w").close()
        o.tick()
        self.assertEqual(o.store.active_runs(), [])
        self.assertFalse(any(launcher.pid_alive(p) for p in pids))
        os.remove(stop)
        os.remove(os.path.join(o.odir, "STOP.NOW"))
        o.tick()
        self.assertEqual(len(o.store.active_runs()), 4)  # resumed, d too

    def test_exit_when_drained(self):
        o = self.make(floors={"daw": 1})
        self.add(o, "a")
        open(os.path.join(o.odir, "STOP"), "w").close()
        o.run(exit_when_drained=True, sleep=lambda s: None)
        self.assertTrue(o.broker.started and o.broker.stopped)

    def test_broker_events_beat_and_suspect(self):
        o = self.make(floors={"daw": 1})
        os.environ["FAKE_CLAUDE_DEFAULT"] = "hang"
        self.add(o, "a")
        o.tick()
        r = o.store.active_runs()[0]
        o.broker.emit("suspect", name=r["name"])
        o.drain_events()
        self.assertEqual(o.store.get_run(r["id"])["suspect"], 1)
        self.clock.offset += 5
        o.broker.emit("registered", name=r["name"])
        o.drain_events()
        got = o.store.get_run(r["id"])
        self.assertEqual(got["suspect"], 0)
        self.assertGreater(got["last_beat"], r["last_beat"] + 4)
        self.assertIn(r["name"], o.broker.expected)
        self.assertEqual(o.broker.claims[r["name"]], ["a/**"])

    def test_numbers_reserved_at_first_launch_and_in_prompt(self):
        o = self.make(floors={"daw": 2})
        for t in ("a", "b"):
            self.add(o, t, wants=("adr", "protocol"))
        log = os.path.join(self.fake_dir, "argv.log")
        self.assertTrue(self.pump(o, lambda: os.path.exists(log)
                                  and len(open(log).readlines()) >= 2))
        a, b = o.store.get_task("a"), o.store.get_task("b")
        self.assertEqual({a["adr_no"], b["adr_no"]}, {11, 12})
        self.assertEqual({a["protocol_no"], b["protocol_no"]}, {11, 12})
        with open(os.path.join(self.fake_dir, "argv.log")) as f:
            prompts = [json.loads(x)[-1] for x in f]
        self.assertTrue(any("adr=11" in p for p in prompts))

    def test_under_floor_wakes_lead_once_and_lead_run_completes(self):
        o = self.make(floors={"daw": 1})
        o.tick()
        o.tick()
        wakes = o.store.tasks(kind="wake")
        self.assertEqual(len(wakes), 1)
        self.assertEqual(wakes[0]["agent"], "lead")
        self.assertTrue(self.pump(o, lambda: state(o, wakes[0]["id"])
                                  == "merged"))
        self.assertIn("pool_under_floor",
                      [ty for _, ty, _ in o.broker.sent])

    def test_launch_failure_counts_as_an_attempt(self):
        o = self.make(floors={"daw": 1})

        class Bad(support.DirWorktrees):
            def ensure(self, *a):
                raise RuntimeError("no git")
        o.worktrees = Bad(self.tmp)
        self.add(o, "a")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "blocked"))
        self.assertEqual(len(o.store.events("a", "launch_failed")), 4)

    def test_status_json_complete_and_always_parseable(self):
        o = self.make(floors={"daw": 1})
        os.environ["FAKE_CLAUDE_DEFAULT"] = "sleep:0.15"
        o.store.add_feature("f1", "daw")
        self.add(o, "a", feature_id="f1")
        path = os.path.join(o.odir, "status.json")
        seen = 0
        end = time.time() + 15
        while time.time() < end and state(o, "a") != "integrating":
            o.tick()
            with open(path) as f:
                st = json.load(f)  # never half-written
            seen += 1
            time.sleep(0.02)
        for key in ("pools", "runs", "features", "waits_on_owner", "cap",
                    "tasks", "privileged_pending", "reservations"):
            self.assertIn(key, st)
        self.assertEqual(st["features"][0]["tasks_total"], 1)
        self.assertEqual(st["cap"], 20)
        self.assertGreater(seen, 2)
        self.assertEqual([x for x in os.listdir(o.odir) if ".tmp." in x], [])


class GitTests(unittest.TestCase):
    def test_wip_checkpoint_and_worktree(self):
        import shutil
        import tempfile
        d = tempfile.mkdtemp(prefix="orch-git-")
        try:
            def git(*a, cwd=d):
                return subprocess.run(["git", "-C", cwd, *a], check=True,
                                      capture_output=True, text=True).stdout
            git("init", "-q", "-b", "main")
            git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q",
                "--allow-empty", "-m", "base")
            wt = launcher.GitWorktrees(d, base="main").ensure("t1", "feat/t1")
            self.assertTrue(os.path.isdir(wt))
            self.assertFalse(heartbeat.wip_checkpoint(wt))  # nothing to save
            with open(os.path.join(wt, "x.txt"), "w") as f:
                f.write("work")
            self.assertTrue(heartbeat.wip_checkpoint(wt))
            self.assertIn("WIP: orchestrator checkpoint",
                          git("log", "-1", "--format=%s", cwd=wt))
            self.assertEqual(git("status", "--porcelain", cwd=wt), "")
            # same worktree is reused, not recreated
            self.assertEqual(launcher.GitWorktrees(d, base="main").ensure(
                "t1", "feat/t1"), wt)
            act = heartbeat.Activity()
            t0 = time.time()
            act.sample("r", wt, os.path.join(d, "none"), 0, t0)
            with open(os.path.join(wt, "y.txt"), "w") as f:
                f.write("more")
            self.assertEqual(act.sample("r", wt, os.path.join(d, "none"), 0,
                                        t0 + 100), t0 + 100)
        finally:
            subprocess.run(["git", "-C", d, "worktree", "prune"],
                           capture_output=True)
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()

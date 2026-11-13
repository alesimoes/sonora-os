"""harness/supervisor.py and supervisor_checks.py: each check against a fake
system (processes, lock directory, disk, notifications) and the real store,
train of an orchestrator built on a temp directory."""
import json
import os
import tempfile
import unittest

import support
from harness import lock_probe, scheduler, supervisor, supervisor_checks as C
from harness.supervisor import Supervisor, Thresholds

NOW = 2_000_000.0


class FakeSys:
    """supervisor_sys.RealSys surface, scripted."""

    def __init__(self):
        self.alive = {}            # pid -> True/False (default True)
        self.entries = []          # lock_entries() rows
        self.killed, self.removed, self.notes = [], [], []
        self.containers, self.ckilled = {}, []   # name -> running
        self.free = 200.0

    def pid_alive(self, pid, kind=None):
        if kind == "other":
            return None
        return self.alive.get(pid, True)

    def kill(self, pid):
        self.killed.append(pid)

    # The real decision (lock_probe) on scripted processes and containers,
    # as a Windows host sees them.
    def entry_alive(self, entry):
        return lock_probe.entry_alive(
            entry, "win", lambda p: self.alive.get(p, True),
            lambda c: self.containers.get(c, False))

    def kill_entry(self, entry):
        return lock_probe.kill_entry(entry, "win", self.killed.append,
                                     self.ckilled.append)

    def lock_entries(self):
        return list(self.entries)

    def lock_remove(self, entry):
        self.removed.append(entry["path"])
        self.entries = [e for e in self.entries if e is not entry]
        return True

    def disk_free_gb(self, path):
        return self.free

    def notify(self, title, text):
        self.notes.append((title, text))
        return True


def entry(role="waits", pid=500, cwd="", cmd="bash build", start=None,
          kind="win", path=None, winpid="same", container=""):
    """A lock row; its Windows pid is the pid unless told otherwise (None:
    an older build-slot.sh that recorded only the MSYS pid)."""
    return {"role": role, "path": path or f"/lock/{role}-{pid}", "pid": pid,
            "kind": kind, "prio": 2, "start": start, "cwd": cwd, "cmd": cmd,
            "claiming": False, "winpid": pid if winpid == "same" else winpid,
            "container": container}


class Case(support.DaemonCase):
    def setUp(self):
        super().setUp()
        self.clock.offset = NOW - __import__("time").time()
        self.o = self.make(dry_run=False)
        self.sys = FakeSys()
        self.th = Thresholds(interval_s=0, notify_repeat_s=600,
                             queue_orphan_s=60, unowned_max_s=600,
                             disk_low_gb=40, disk_critical_gb=15,
                             disk_janitor_every_s=3600)
        self.sv = Supervisor(self.o, self.sys, th=self.th, clock=self.clock)
        self.o.supervisor = self.sv

    def now(self):
        return self.clock()

    def sweep(self):
        self.sv.tick(self.now(), force=True)
        return {a["id"]: a for a in self.sv.alerts.rows()}

    def wt(self, name):
        p = os.path.join(self.tmp, "wt", name)
        os.makedirs(p, exist_ok=True)
        return p

    def add_run(self, rid="t1-r1", task="t1", worktree=None, suspect=0,
                beat_ago=0, ended=None, name="eng-1"):
        if self.o.store.get_task(task) is None:
            self.o.store.add_task(task, "daw", "rt-engineer", files=["x/**"])
        self.o.store.add_run(
            id=rid, task_id=task, role="engineer", name=name, state="building",
            pid=7000, started=self.now() - beat_ago - 10,
            last_beat=self.now() - beat_ago, result_path="r", stream_path="s",
            worktree=worktree or self.wt(task))
        if suspect:
            self.o.store.set_run(rid, suspect=1)
        if ended:
            self.o.store.set_run(rid, ended=self.now(), outcome="finished")
        return self.o.store.get_run(rid)

    def actions(self, check=None):
        return [a for a in self.sv.actions if not check or a.check == check]


# -- runs -------------------------------------------------------------------
class Runs(Case):
    def test_suspect_run_is_nudged_once_and_shown(self):
        self.add_run(suspect=1, beat_ago=1000)
        al = self.sweep()
        self.sweep()
        wakes = [s for s in self.o.broker.sent if s[1] == "wake"]
        self.assertEqual(len(wakes), 1)
        self.assertEqual(wakes[0][0], "eng-1")
        self.assertIn("run:t1-r1", al)
        self.assertEqual(al["run:t1-r1"]["level"], "warn")
        self.assertEqual(self.sys.notes, [])   # warn: no toast

    def test_run_waiting_in_lock_queue_is_building_not_stuck(self):
        r = self.add_run(suspect=1, beat_ago=1000)
        self.sys.entries = [entry("waits", 501, cwd=r["worktree"],
                                  start=self.now() - 60)]
        al = self.sweep()
        self.assertEqual([s for s in self.o.broker.sent if s[1] == "wake"],
                         [])
        self.assertNotIn("run:t1-r1", al)

    def test_scheduler_counts_a_waiter_as_building(self):
        listing = ("waits pid=1 kind=win user=a prio=2 start=1 "
                   "cwd=/c/src/.claude/worktrees/t1 cmd=bash tools/build-ui.sh")
        self.assertTrue(scheduler.holder_in_list(
            listing, r"C:\src\.claude\worktrees\t1"))
        self.assertFalse(scheduler.holder_in_list(
            listing, r"C:\src\.claude\worktrees\t2"))


# -- the lock ---------------------------------------------------------------
class Lock(Case):
    def test_dead_pid_is_reaped(self):
        self.sys.entries = [entry("waits", 600, cwd="/x",
                                  start=self.now() - 5)]
        self.sys.alive[600] = False
        self.sweep()
        self.assertEqual(self.sys.removed, ["/lock/waits-600"])
        a = self.actions("lock")[0]
        self.assertIn("pid dead", a.reason)

    def test_process_left_by_an_ended_run_is_killed(self):
        r = self.add_run(ended=True)
        self.sys.entries = [entry("holds", 601, cwd=r["worktree"],
                                  start=self.now() - 5)]
        self.sweep()
        self.assertEqual(self.sys.killed, [601])
        self.assertEqual(self.sys.removed, ["/lock/holds-601"])
        self.assertIn("left behind by ended run t1-r1",
                      self.actions("lock")[0].reason)

    def test_live_run_worktree_is_left_alone(self):
        r = self.add_run()
        self.sys.entries = [entry("holds", 602, cwd=r["worktree"],
                                  start=self.now() - 5)]
        self.sweep()
        self.assertEqual(self.sys.killed, [])

    def test_long_waiter_and_holder_alert(self):
        self.sys.entries = [
            entry("waits", 605, cwd="/elsewhere", start=self.now() - 3 * 3600),
            entry("holds", 606, cwd="/elsewhere", start=self.now() - 4 * 3600)]
        al = self.sweep()
        self.assertIn("lock:waits:605", al)
        self.assertIn("lock:holds:606", al)
        self.assertEqual(self.sys.killed, [])
        self.assertEqual(len(self.sys.notes), 2)

    def test_pid_of_another_host_kind_is_never_reaped(self):
        self.sys.entries = [entry("waits", 607, kind="other", cwd="/y",
                                  start=self.now() - 5)]
        self.sweep()
        self.assertEqual(self.sys.removed, [])

    def test_msys_pid_alone_is_never_taken_for_a_windows_pid(self):
        # 2026-11-13: the supervisor probed Git bash's $$ as a Windows pid,
        # found nothing and reaped live holders; a second build then ran
        # beside the first.
        self.sys.entries = [entry("holds", 150963, cwd="/x", winpid=None,
                                  start=self.now() - 5)]
        self.sys.alive[150963] = False
        self.sweep()
        self.assertEqual(self.sys.removed, [])

    def test_dead_shell_with_a_running_container_keeps_its_slot(self):
        self.sys.entries = [entry("holds", 610, cwd="/x", container="c1",
                                  start=self.now() - 5)]
        self.sys.alive[610] = False
        self.sys.containers["c1"] = True
        self.sweep()
        self.assertEqual(self.sys.removed, [])
        self.sys.containers["c1"] = False
        self.sweep()
        self.assertEqual(self.sys.removed, ["/lock/holds-610"])

    def test_docker_that_does_not_answer_keeps_the_slot(self):
        self.sys.entries = [entry("holds", 611, cwd="/x", container="c2",
                                  start=self.now() - 5)]
        self.sys.alive[611] = False
        self.sys.containers["c2"] = None
        self.sweep()
        self.assertEqual(self.sys.removed, [])

    def test_ended_run_holder_without_a_windows_pid_is_not_killed_blind(self):
        # Its MSYS pid may be any Windows process's number: taskkill on it
        # was a random kill. Nothing reachable: the slot stays (the lock's
        # own reaper probes the MSYS pid from Git bash).
        r = self.add_run(ended=True)
        self.sys.entries = [entry("holds", 612, cwd=r["worktree"],
                                  winpid=None, start=self.now() - 5)]
        self.sweep()
        self.assertEqual(self.sys.killed, [])
        self.assertEqual(self.sys.removed, [])

    def test_ended_run_holder_container_is_stopped_with_it(self):
        r = self.add_run(ended=True)
        self.sys.entries = [entry("holds", 613, cwd=r["worktree"],
                                  container="c3", start=self.now() - 5)]
        self.sweep()
        self.assertEqual(self.sys.killed, [613])
        self.assertEqual(self.sys.ckilled, ["c3"])
        self.assertEqual(self.sys.removed, ["/lock/holds-613"])

    def test_claim_without_owner_is_reaped_after_grace(self):
        e = entry("holds", None, start=self.now() - 300)
        e["claiming"] = True
        self.sys.entries = [e]
        self.sweep()
        self.assertEqual(self.sys.removed, [e["path"]])


# -- queues -------------------------------------------------------------------
class Queues(Case):
    def train(self, **rows):
        self.o.store.set_meta("train", json.dumps(rows))

    def test_open_pr_the_sweep_missed_is_adopted_after_the_grace(self):
        self.forge.add_pr(5, "feat/x", files=["ui/a.cpp"])
        self.sweep()
        self.assertIsNone(self.o.store.get_feature("pr-5"))   # the daemon's turn
        self.clock.offset += self.th.adopt_grace_s + 1
        self.sweep()
        self.assertEqual(self.o.store.get_feature("pr-5")["pr"], 5)
        self.assertEqual(self.actions("queues")[0].what, "adopt")
        self.assertIn("auto", self.o.store.events("pr-5")[0]["detail"])
        self.assertEqual(self.sweep().get("queues:unadopted"), None)

    def test_draft_is_not_adopted_but_a_stacked_pr_is(self):
        # ADR-0061 section 6: a PR on another PR is adopted and retargeted.
        self.forge.add_pr(6, "feat/d", draft=True)
        self.forge.add_pr(7, "feat/s", base="feat/d")
        self.sweep()
        self.clock.offset += self.th.adopt_grace_s + 1
        self.sweep()
        self.assertIsNone(self.o.store.get_feature("pr-6"))
        self.assertEqual(self.o.store.get_feature("pr-7")["pr"], 7)


# -- PRs ----------------------------------------------------------------------
class PRs(Case):
    def red(self, tid="pr-9", age=3600, backoff=0):
        """A PR red `age` seconds ago (the clock moves: set_task stamps
        `updated` itself) with no fixer run."""
        s = self.o.store
        s.add_task(tid, "daw", "rt-engineer", state="pr_open", branch="b")
        s.set_feature(tid, pr=9)
        s.advance(tid, "ci_fix")
        s.set_task(tid, fix_kind="ci",
                   not_before=self.now() + age + backoff if backoff else 0)
        self.clock.offset += age

    def test_red_pr_in_backoff_is_woken(self):
        self.red(backoff=5000)
        al = self.sweep()
        self.assertLessEqual(self.o.store.get_task("pr-9")["not_before"],
                             self.now())
        self.assertIn("backoff cleared", self.actions("prs")[0].reason)
        self.assertNotIn("unowned:pr-9", al)

    def test_red_pr_with_no_fixer_alerts_with_the_reason(self):
        self.red()
        al = self.sweep()
        self.assertIn("unowned:pr-9", al)
        self.assertIn("no fixer", al["unowned:pr-9"]["text"])
        self.assertEqual(len(self.sys.notes), 1)

    def test_recent_red_and_owned_red_are_quiet(self):
        self.red(age=10)
        self.assertNotIn("unowned:pr-9", self.sweep())
        self.clock.offset += 3600
        self.add_run(rid="pr-9-r1", task="pr-9")
        self.assertNotIn("unowned:pr-9", self.sweep())

    def test_backoff_writes_do_not_reset_the_wait(self):
        self.red(age=10)
        self.sweep()                       # first seen now
        self.clock.offset += 1000
        self.o.store.set_task("pr-9", attempts=2)   # stamps `updated`
        self.assertIn("unowned:pr-9", self.sweep())

    def test_conflict_names_the_integrator(self):
        self.red(age=0)
        self.o.store.set_task("pr-9", fix_kind="conflict")
        self.clock.offset += 3600
        self.assertIn("no integrator", self.sweep()["unowned:pr-9"]["text"])

    def test_blocked_pr_is_a_warning(self):
        s = self.o.store
        s.add_task("pr-8", "daw", "rt-engineer", state="pr_open", branch="b")
        s.set_feature("pr-8", pr=8)
        s.advance("pr-8", "blocked", note="closed without merge")
        al = self.sweep()
        self.assertEqual(al["blocked:pr-8"]["level"], "warn")


# -- disk -----------------------------------------------------------------------
class Disk(Case):
    def paused(self):
        return self.o.paused()

    def test_low_disk_wakes_the_janitor_once_per_period(self):
        self.sys.free = 30
        al = self.sweep()
        self.sweep()
        wakes = [t for t in self.o.store.tasks(kind="wake")
                 if t["agent"] == "janitor"]
        self.assertEqual(len(wakes), 1)
        self.assertEqual(al["disk:low"]["level"], "warn")
        self.assertEqual(self.paused(), set())

    def test_critical_disk_pauses_build_pools_and_resumes(self):
        self.sys.free = 10
        al = self.sweep()
        self.assertEqual(self.paused(), {"build", "qa"})
        self.assertIn("disk:critical", al)
        self.assertEqual(len(self.sys.notes), 1)
        self.sys.free = 100
        al = self.sweep()
        self.assertEqual(self.paused(), set())
        self.assertNotIn("disk:critical", al)
        self.assertEqual(self.o.store.get_meta(C.PAUSED_META), "")

    def test_pools_paused_by_hand_stay_paused(self):
        supervisor.write_atomic(self.o._p("paused.json"), ["build", "plan"])
        self.sys.free = 10
        self.sweep()
        self.assertEqual(self.paused(), {"build", "qa", "plan"})
        self.sys.free = 100
        self.sweep()
        self.assertEqual(self.paused(), {"build", "plan"})


# -- the supervisor itself -------------------------------------------------------
class Core(Case):
    def test_alert_notifies_once_then_repeats_after_the_period(self):
        self.sys.free = 10
        self.sweep()
        self.sweep()
        self.assertEqual(len(self.sys.notes), 1)
        self.clock.offset += 601
        self.sweep()
        self.assertEqual(len(self.sys.notes), 2)

    def test_healed_alert_disappears(self):
        self.sys.free = 10
        self.assertIn("disk:critical", self.sweep())
        self.sys.free = 100
        self.assertNotIn("disk:critical", self.sweep())

    def test_outer_alerts_are_folded_in(self):
        supervisor.write_outer_alerts(self.o.odir, [
            {"id": "daemon:stale-code", "text": "behind main",
             "check": "daemon"}])
        al = self.sweep()
        self.assertIn("outer:daemon:stale-code", al)

    def test_status_json_carries_alerts_actions_and_pid(self):
        self.sys.free = 10
        self.sweep()
        self.o._write_status(self.now(), False, False, set())
        with open(self.o._p("status.json"), encoding="utf-8") as f:
            st = json.load(f)
        self.assertEqual(st["daemon_pid"], os.getpid())
        self.assertEqual([a["id"] for a in st["alerts"]], ["disk:critical"])
        self.assertTrue(st["supervisor"]["actions"])

    def test_actions_are_logged_with_reasons(self):
        self.sys.free = 10
        self.sweep()
        with open(self.sv.log_path, encoding="utf-8") as f:
            log = f.read()
        self.assertIn("disk: pause pools build,qa -- 10 GB free", log)
        kinds = [e["kind"] for e in self.o.store.events("supervisor")]
        self.assertIn("disk:pause pools build,qa", kinds)

    def test_a_crashing_check_does_not_stop_the_others(self):
        def boom(sv, now):
            raise RuntimeError("x")
        self.sv.checks = (boom, C.check_disk)
        self.sys.free = 10
        al = self.sweep()
        self.assertIn("disk:critical", al)
        self.assertEqual(self.actions()[0].what, "crashed")
        self.assertFalse(self.actions()[0].ok)

    def test_sweeps_only_every_interval(self):
        self.sv.th.interval_s = 100
        self.assertTrue(self.sv.tick(self.now()))
        self.assertFalse(self.sv.tick(self.now() + 50))
        self.assertTrue(self.sv.tick(self.now() + 100))

    def test_dry_run_daemon_does_not_sweep(self):
        o = self.make(dry_run=True)
        o.supervisor = Supervisor(o, self.sys, th=self.th, clock=self.clock)
        o.tick()
        self.assertEqual(o.supervisor.last_sweep, 0.0)


class Config(unittest.TestCase):
    def test_thresholds_from_toml_table(self):
        th = Thresholds.from_dict({"interval_s": 60, "notify": 0,
                                   "unknown": 1})
        self.assertEqual((th.interval_s, th.notify), (60.0, False))
        self.assertEqual(Thresholds.from_dict(None).disk_low_gb, 40.0)

    def test_load_config_keeps_the_supervisor_table(self):
        if scheduler.tomllib is None:
            self.skipTest("no tomllib")
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "o.toml")
            with open(p, "w") as f:
                f.write("cap = 3\n[supervisor]\ninterval_s = 45\n")
            cfg = scheduler.load_config(p)
        self.assertEqual(cfg.supervisor, {"interval_s": 45})
        self.assertEqual(Thresholds.from_dict(cfg.supervisor).interval_s, 45)


if __name__ == "__main__":
    unittest.main()

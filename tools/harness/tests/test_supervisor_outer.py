"""harness/supervisor_outer.py: the process that watches the daemon, against
a fake system, a scripted git and a recording spawn."""
import json
import os
import shutil
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import supervisor
from harness.supervisor import Thresholds
from harness.supervisor_outer import Outer
from test_supervisor import FakeSys

NOW = 3_000_000.0


class OuterCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sv-outer-")
        self.odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(self.odir)
        self.sys = FakeSys()
        self.now = NOW
        self.spawned = []
        self.git_out = {"fetch": "", "diff": ""}
        th = Thresholds(daemon_stale_s=300, disk_critical_gb=15,
                        notify_repeat_s=3600)
        self.outer = Outer(self.tmp, self.sys, th=th, clock=lambda: self.now,
                           spawn=self._spawn, git=self._git)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _spawn(self, root, log):
        self.spawned.append((root, log))
        return 999

    def _git(self, root, *args):
        return self.git_out.get(args[0])

    def status(self, age=10, pid=42):
        supervisor.write_atomic(os.path.join(self.odir, "status.json"),
                                {"updated_ts": self.now - age,
                                 "daemon_pid": pid})

    def sweep(self):
        return {a["id"]: a for a in self.outer.tick(self.now)}

    def test_fresh_status_is_quiet(self):
        self.status()
        self.assertEqual(self.sweep(), {})
        with open(os.path.join(self.odir, "supervisor-alerts.json")) as f:
            self.assertEqual(json.load(f), [])

    def test_dead_daemon_is_restarted(self):
        self.status(age=1000)
        self.sys.alive[42] = False
        al = self.sweep()
        self.assertEqual(len(self.spawned), 1)
        self.assertEqual(self.spawned[0][0], self.tmp)
        self.assertIn("daemon:restarted", al)
        self.assertIn("pid 42 dead", al["daemon:restarted"]["text"])
        self.assertEqual(len(self.sys.notes), 1)
        self.assertTrue(any(w == "restart daemon"
                            for w, _, _ in self.outer.actions))

    def test_no_status_file_means_restart(self):
        al = self.sweep()
        self.assertEqual(len(self.spawned), 1)
        self.assertIn("no status.json", al["daemon:restarted"]["text"])

    def test_hung_daemon_is_killed_on_the_second_sweep(self):
        self.status(age=1000)
        al = self.sweep()
        self.assertIn("daemon:hung", al)
        self.assertEqual((self.sys.killed, self.spawned), ([], []))
        al = self.sweep()
        self.assertEqual(self.sys.killed, [42])
        self.assertEqual(len(self.spawned), 1)
        self.assertIn("daemon:restarted", al)

    def test_fresh_status_resets_the_hung_count(self):
        self.status(age=1000)
        self.sweep()
        self.status(age=1)
        self.sweep()
        self.status(age=1000)
        al = self.sweep()
        self.assertIn("daemon:hung", al)
        self.assertEqual(self.sys.killed, [])

    def test_no_restart_while_the_disk_is_critical(self):
        self.status(age=1000)
        self.sys.alive[42] = False
        self.sys.free = 5
        al = self.sweep()
        self.assertEqual(self.spawned, [])
        self.assertIn("daemon:down", al)
        self.assertIn("disk critical", al["daemon:down"]["text"])
        self.assertIn("disk:critical", al)

    def test_restart_can_be_switched_off(self):
        self.outer.th.daemon_restart = False
        self.status(age=1000)
        self.sys.alive[42] = False
        al = self.sweep()
        self.assertEqual(self.spawned, [])
        self.assertIn("restart is off", al["daemon:down"]["text"])

    def test_spawn_failure_is_an_alert(self):
        def bad(root, log):
            raise OSError("no python")
        self.outer.spawn = bad
        self.status(age=1000)
        self.sys.alive[42] = False
        al = self.sweep()
        self.assertIn("restart failed", al["daemon:down"]["text"])

    def test_stale_harness_code_is_named(self):
        self.status()
        self.git_out["diff"] = "tools/harness/train.py\ntools/orchestrator.py\n"
        al = self.sweep()
        self.assertIn("daemon:stale-code", al)
        self.assertIn("2 harness file(s)", al["daemon:stale-code"]["text"])
        self.assertIn("tools/harness/train.py", al["daemon:stale-code"]["text"])
        self.git_out["diff"] = ""
        self.assertNotIn("daemon:stale-code", self.sweep())

    def test_fetch_failure_is_a_warning_only(self):
        self.status()
        self.git_out["fetch"] = None
        al = self.sweep()
        self.assertEqual(al["daemon:fetch"]["level"], "warn")
        self.assertEqual(self.sys.notes, [])

    def test_fetch_runs_once_per_period(self):
        self.status()
        calls = []
        self.outer.git = lambda root, *a: calls.append(a[0]) or ""
        self.sweep()
        self.now += 60
        self.sweep()
        self.assertEqual(calls.count("fetch"), 1)
        self.now += 600
        self.sweep()
        self.assertEqual(calls.count("fetch"), 2)

    def test_run_once(self):
        self.status()
        rows = self.outer.run(once=True)
        self.assertEqual(rows, [])

    def test_log_lines_carry_the_reason(self):
        self.status(age=1000)
        self.sys.alive[42] = False
        self.sweep()
        with open(os.path.join(self.tmp, ".harness", "supervisor.log")) as f:
            log = f.read()
        self.assertIn("outer: restart daemon -- status.json 16 min old, "
                      "pid 42 dead; new pid 999", log)


if __name__ == "__main__":
    unittest.main()

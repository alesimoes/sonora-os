"""harness/docker_heal: Docker down is an alert of its own and, after a
while, Docker Desktop is restarted a bounded number of times (2026-11-13:
the cycle stood 3 h 40 with zero agents, the status said only "paused").
A fake system and fake process calls: no real Docker here."""
import os
import tempfile
import types
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import docker_guard, docker_heal
from harness.supervisor import Supervisor, Thresholds

T0 = 2_000_000.0


class Events:
    def __init__(self):
        self.rows = []

    def event(self, *a):
        self.rows.append(a)


class FakeSys:
    def __init__(self, result=True):
        self.restarts, self.notes, self.result = 0, [], result

    def docker_restart(self):
        self.restarts += 1
        return self.result

    def notify(self, title, text):
        self.notes.append((title, text))
        return True


class Heal(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="dh-")
        self.addCleanup(support.shutil.rmtree, self.tmp, True)
        self.t = T0
        self.up = True
        self.store = Events()
        self.guard = docker_guard.Guard(self.store, lambda: self.up)
        self.o = types.SimpleNamespace(store=self.store, docker=self.guard,
                                       hdir=self.tmp, odir=self.tmp)
        self.sys = FakeSys()
        self.th = Thresholds(interval_s=0, docker_restart_after_s=180,
                             docker_restart_gap_s=300, docker_restart_max=3,
                             notify_repeat_s=3600)
        self.sv = Supervisor(self.o, self.sys, th=self.th,
                             clock=lambda: self.t,
                             checks=(docker_heal.check_docker,))

    def at(self, dt, up=None):
        """Moves the clock to T0+dt, probes, sweeps; returns the alerts."""
        self.t = T0 + dt
        if up is not None:
            self.up = up
        self.guard.checked = None   # probe now
        self.guard.held(self.t)
        self.sv.tick(self.t, force=True)
        return {a["id"]: a for a in self.sv.alerts.rows()}

    def attempts(self):
        return [a for a in self.sv.actions if a.what.startswith("restart")]

    def test_up_raises_nothing(self):
        self.assertEqual(self.at(0), {})
        self.assertEqual(self.sys.restarts, 0)

    def test_a_short_outage_is_a_warning_without_restart_or_toast(self):
        al = self.at(0, up=False)
        self.assertEqual(al["docker:down"]["level"], "warn")
        self.assertIn("Docker down since", al["docker:down"]["text"])
        self.assertEqual((self.sys.restarts, self.sys.notes), (0, []))

    def test_after_the_bound_it_alerts_once_and_restarts(self):
        self.at(0, up=False)
        al = self.at(180)
        self.assertEqual(al["docker:down"]["level"], "alert")
        self.assertEqual(self.sys.restarts, 1)
        self.assertEqual(len(self.sys.notes), 1)
        self.at(240)                       # inside the gap: no new attempt
        self.assertEqual(self.sys.restarts, 1)
        self.assertEqual(len(self.sys.notes), 1)   # one toast per hour
        ev = [r for r in self.store.rows if r[0] == "supervisor"
              and r[1].startswith("docker:restart")]
        self.assertEqual(len(ev), 1)       # each attempt is an event

    def test_attempts_are_bounded_then_it_stops_and_says_so(self):
        self.at(0, up=False)
        for dt in range(180, 180 + 10 * 300, 300):
            al = self.at(dt)
        self.assertEqual(self.sys.restarts, 3)
        self.assertIn("no more tries", al["docker:down"]["text"])
        gave_up = [a for a in self.sv.actions if a.what.startswith("stop")]
        self.assertEqual(len(gave_up), 1)

    def test_back_up_clears_the_alert_and_a_new_outage_gets_attempts(self):
        self.at(0, up=False)
        self.at(180)
        self.assertEqual(self.at(400, up=True), {})
        self.at(4000, up=False)
        self.at(4200)
        self.assertEqual(self.sys.restarts, 2)

    def test_a_flapping_docker_is_held_to_the_hourly_bound(self):
        dt = 0
        for _ in range(5):                 # down, restarted, up, down again
            self.at(dt, up=False)
            self.at(dt + 180)
            self.at(dt + 240, up=True)
            dt += 300
        self.assertEqual(self.sys.restarts, 3)

    def test_without_a_restart_surface_it_only_alerts(self):
        self.sv.sys = types.SimpleNamespace(notify=self.sys.notify)
        self.at(0, up=False)
        al = self.at(600)
        self.assertIn("by hand", al["docker:down"]["text"])

    def test_switched_off_by_config_it_only_alerts(self):
        self.th.docker_restart = False
        self.at(0, up=False)
        al = self.at(600)
        self.assertEqual(self.sys.restarts, 0)
        self.assertIn("by hand", al["docker:down"]["text"])

    def test_a_failed_attempt_is_logged_failed(self):
        self.sys.result = None
        self.at(0, up=False)
        self.at(180)
        self.assertFalse(self.attempts()[0].ok)

    def test_thresholds_read_from_the_toml_table(self):
        th = Thresholds.from_dict({"docker_restart": False,
                                   "docker_restart_after_s": 60})
        self.assertFalse(th.docker_restart)
        self.assertEqual(th.docker_restart_after_s, 60.0)


class Restart(unittest.TestCase):
    """The Windows sequence that works by hand, with fake process calls."""

    def setUp(self):
        self.calls, self.started = [], []
        fd, self.exe = tempfile.mkstemp(suffix=".exe")
        os.close(fd)
        self.addCleanup(os.remove, self.exe)

    def run_(self, cmd, **kw):
        self.calls.append(cmd)
        if cmd[0] == "taskkill" and "docker.exe" in cmd:
            raise OSError("not running")   # a missing process is fine

    def popen(self, cmd, **kw):
        self.started.append(cmd)

    def test_stops_every_process_then_wsl_then_starts_desktop(self):
        r = docker_heal.restart_docker_desktop(
            self.run_, self.popen, exe=self.exe, is_windows=True)
        self.assertTrue(r)
        killed = [c[-1] for c in self.calls if c[0] == "taskkill"]
        self.assertEqual(killed, [p + ".exe" for p in
                                  docker_heal.DOCKER_PROCS])
        self.assertEqual(self.calls[-1], ["wsl", "--shutdown"])
        self.assertEqual(self.started, [[self.exe]])

    def test_not_windows_or_not_installed_is_not_supported(self):
        self.assertIsNone(docker_heal.restart_docker_desktop(
            self.run_, self.popen, exe=self.exe, is_windows=False))
        self.assertIsNone(docker_heal.restart_docker_desktop(
            self.run_, self.popen, exe=self.exe + ".none", is_windows=True))
        self.assertEqual((self.calls, self.started), ([], []))


if __name__ == "__main__":
    unittest.main()

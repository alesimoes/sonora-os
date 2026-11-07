"""Dashboard self-update and the supervisor's dashboard check, with fakes."""
import os
import subprocess
import sys
import unittest

import support  # noqa: F401

import dashboard_selfupdate as du
from harness import supervisor_dash


def cp(out="", code=0, err=""):
    return subprocess.CompletedProcess([], code, out, err)


class FakeGit:
    def __init__(self, diff="", dirty="", fetch=0, checkout=0):
        self.diff, self.dirty, self.fetch = diff, dirty, fetch
        self.checkout, self.calls = checkout, []

    def __call__(self, args, cwd, timeout=0):
        self.calls.append(args)
        if args[0] == "fetch":
            return cp(code=self.fetch)
        if args[0] == "diff":
            return cp(self.diff)
        if args[0] == "status":
            return cp(self.dirty)
        return cp(code=self.checkout, err="boom")


class SelfUpdate(unittest.TestCase):
    def run_once(self, git):
        logs, restarts = [], []
        r = du.update_once("/r", git, restarts.append, ["d.py", "--port", "1"],
                           logs.append)
        return r, logs, restarts

    def test_no_change_does_nothing(self):
        r, logs, restarts = self.run_once(FakeGit())
        self.assertEqual((r, restarts), ("current", []))

    def test_fetch_failure_is_current(self):
        r, _, restarts = self.run_once(FakeGit(diff="x", fetch=1))
        self.assertEqual((r, restarts), ("current", []))

    def test_change_moves_checkout_and_reexecs_with_argv(self):
        git = FakeGit(diff="tools/dashboard.py\n")
        r, logs, restarts = self.run_once(git)
        self.assertEqual(r, "restarted")
        self.assertEqual(restarts, [["d.py", "--port", "1"]])
        self.assertIn(["checkout", "-q", "--detach", "origin/main"], git.calls)
        self.assertIn("tools/dashboard.py", logs[0])

    def test_dirty_checkout_is_refused_and_logged(self):
        r, logs, restarts = self.run_once(
            FakeGit(diff="tools/dashboard.py\n", dirty=" M tools/dashboard.py"))
        self.assertEqual((r, restarts), ("blocked", []))
        self.assertIn("refused", logs[0])

    def test_checkout_failure_blocks(self):
        r, logs, restarts = self.run_once(
            FakeGit(diff="tools/dashboard.py\n", checkout=1))
        self.assertEqual((r, restarts), ("blocked", []))

    def test_loop_survives_an_exception(self):
        n = []

        def sleep(_):
            n.append(1)
            if len(n) > 2:
                raise KeyboardInterrupt

        def boom(*a, **k):
            raise RuntimeError("x")

        with self.assertRaises(KeyboardInterrupt):
            du.loop("/r", 1, sleep, git=boom)
        self.assertEqual(len(n), 3)


class FakeSV:
    def __init__(self, up):
        self.mem, self.acts = {}, []
        self.up = up
        self.o = type("O", (), {"root": "/repo/.claude/worktrees/agent-x"})()
        self.sys = type("S", (), {})()
        self.sys.dash_up = lambda port: self.up
        self.sys.dash_start = lambda port, root: self.acts.append(root) or ["c"]

    def remember(self, c, k, d=None):
        return self.mem.setdefault(c, {}).setdefault(k, d)

    def act(self, check, what, reason, fn=None):
        fn()


class DashCheck(unittest.TestCase):
    def test_restart_after_two_failed_sweeps_then_rate_limited(self):
        sv = FakeSV(False)
        supervisor_dash.check_dashboard(sv, 1000)
        self.assertEqual(sv.acts, [])
        supervisor_dash.check_dashboard(sv, 1030)
        self.assertEqual(len(sv.acts), 1)
        supervisor_dash.check_dashboard(sv, 1060)
        self.assertEqual(len(sv.acts), 1)
        supervisor_dash.check_dashboard(sv, 1400)
        self.assertEqual(len(sv.acts), 2)

    def test_up_resets_and_skips(self):
        sv = FakeSV(True)
        supervisor_dash.check_dashboard(sv, 1)
        self.assertEqual(sv.acts, [])
        self.assertNotIn("dash", sv.mem)

    def test_sys_without_probe_is_skipped(self):
        sv = FakeSV(False)
        sv.sys = object()
        supervisor_dash.check_dashboard(sv, 1)

    def test_dash_root_is_sibling_of_daemon_worktree(self):
        self.assertEqual(
            supervisor_dash.dash_root(os.path.abspath(os.path.join("r", "worktrees", "agent-x")), {}),
            os.path.abspath(os.path.join("r", "worktrees", "dash-live")))
        self.assertEqual(supervisor_dash.dash_root("/x", {"SONORA_DASH_ROOT": "/d"}), "/d")

    def test_start_runs_script_with_port_or_none(self):
        import tempfile
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "tools"))
        calls = []
        env = {"SONORA_DASH_ROOT": d}
        self.assertIsNone(supervisor_dash.start("/x", 8765, calls.append, env))
        open(os.path.join(d, "tools", "dashboard.py"), "w").close()
        cmd = supervisor_dash.start("/x", 8765, lambda c, **k: calls.append(c), env)
        self.assertEqual(cmd[-2:], ["--port", "8765"])
        self.assertEqual(calls, [cmd])


if __name__ == "__main__":
    unittest.main()

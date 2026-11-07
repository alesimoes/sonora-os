"""Dashboard side of the daemon door: the three actions, the daemon-alive
check on the new paths, and the failed steps shown on train rows."""
import json
import os
import time
import unittest
from unittest import mock

import support  # noqa: F401
from test_dashboard_actions import Base

import dashboard_actions as A
import dashboard_actions_door  # noqa: F401  (registers)
import dashboard_daemon
import dashboard_orch
from harness import cli_mailbox

HEAD = "c" * 40


class DaemonAlive(Base):
    def test_fresh_status_or_events_or_lock_pid(self):
        self.assertFalse(dashboard_daemon.daemon_alive())
        self.write("status.json", {})
        self.assertTrue(dashboard_daemon.daemon_alive())
        old = time.time() - 3600
        os.utime(os.path.join(self.odir, "status.json"), (old, old))
        self.assertFalse(dashboard_daemon.daemon_alive())
        with open(os.path.join(self.odir, "events.jsonl"), "w") as f:
            f.write("{}\n")
        self.assertTrue(dashboard_daemon.daemon_alive())

    def test_lock_with_a_live_daemon_pid_counts(self):
        os.makedirs(os.path.join(self.tmp, ".harness"))
        with open(os.path.join(self.tmp, ".harness", "daemon.lock"), "w") as f:
            json.dump({"pid": 9420, "role": "daemon"}, f)
        with mock.patch("harness.launcher.pid_alive", lambda p: p == 9420):
            self.assertTrue(dashboard_daemon.daemon_alive())
        with mock.patch("harness.launcher.pid_alive", lambda p: False):
            self.assertFalse(dashboard_daemon.daemon_alive())


class Door(Base):
    def setUp(self):
        super().setUp()
        self.write("status.json", {"train": [
            {"pr": 7, "head": HEAD, "phase": "fixer"},
            {"pr": 8, "head": "d", "phase": "CI", "fixer": "fixer-1"}]})
        self.write("state.json", {"meta": {"localci:" + HEAD: json.dumps(
            {"state": "fail", "summary": "ok   tree (1 s)\nFAIL tsan (2 s): x"})}})
        os.makedirs(os.path.join(self.tmp, ".harness"))

    def requests(self):
        return cli_mailbox.take(os.path.join(self.tmp, ".harness"))

    def test_all_three_need_confirmation_and_a_daemon(self):
        for name, p in (("pr.regate", {"pr": 7}), ("pr.rerun_step", {"pr": 7, "step": "tsan"}),
                        ("pr.assign", {"pr": 7, "as": "fixer"})):
            with mock.patch("dashboard_actions_door.daemon_alive", lambda: False):
                self.assertEqual(A.submit(name, p)[0], 400, "no daemon running")
        with mock.patch("dashboard_actions_door.daemon_alive", lambda: True):
            for name, p in (("pr.regate", {"pr": 7}), ("pr.rerun_step", {"pr": 7, "step": "tsan"}),
                            ("pr.assign", {"pr": 7, "as": "fixer"})):
                self.assertEqual(A.submit(name, p)[0], 409, name)
        self.assertEqual(self.requests(), [])

    def test_requests_reach_the_mailbox_and_are_audited(self):
        with mock.patch("dashboard_actions_door.daemon_alive", lambda: True):
            self.go("pr.regate", {"pr": 7}, True)
            code, _, job = self.go("pr.rerun_step", {"pr": 7, "step": "tsan"}, True)
            self.go("pr.assign", {"pr": 7, "as": "integrator"}, True)
        self.assertEqual(job["state"], "done")
        self.assertEqual(sorted(k for k, _ in self.requests()), ["assign", "regate", "rerun-step"])
        with open(os.path.join(self.tmp, "audit.log"), encoding="utf-8") as f:
            self.assertEqual(len(f.read().strip().splitlines()), 3)

    def test_validation(self):
        with mock.patch("dashboard_actions_door.daemon_alive", lambda: True):
            self.assertEqual(A.submit("pr.regate", {"pr": 99}, True)[0], 400)
            self.assertEqual(A.submit("pr.rerun_step", {"pr": 7, "step": "asan"}, True)[0], 400)
            self.assertEqual(A.submit("pr.assign", {"pr": 7, "as": "root"}, True)[0], 400)
            self.assertEqual(A.submit("pr.assign", {"pr": 8, "as": "fixer"}, True)[0], 400)

    def test_the_dashboard_never_writes_the_daemon_state(self):
        before = open(os.path.join(self.odir, "state.json"), "rb").read()
        with mock.patch("dashboard_actions_door.daemon_alive", lambda: True):
            self.go("pr.regate", {"pr": 7}, True)
        self.assertEqual(open(os.path.join(self.odir, "state.json"), "rb").read(), before)

    def test_train_rows_carry_their_failed_steps(self):
        o = dashboard_orch.clean_orch({"train": [{"pr": 7, "head": HEAD}, {"pr": 8, "head": "d"}]}, {7, 8})
        self.assertEqual([r["failed_steps"] for r in o["train"]], [["tsan"], []])


if __name__ == "__main__":
    unittest.main()

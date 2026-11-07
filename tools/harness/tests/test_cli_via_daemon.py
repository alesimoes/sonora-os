"""state.json keeps one writer (2026-11-07: `adopted_pr` events existed for
#471-#473 but their features vanished, overwritten by the daemon's next
save). The lost adoption is reproduced first, then each door that closes it:
the store keeps another writer's additions, CLI commands go to a running
daemon's mailbox, and the daemon lock decides who writes."""
import contextlib
import io
import json
import os
import shutil
import tempfile
import unittest

import support
from harness import autoadopt, cli_mailbox, daemonlock
from harness.store import Store


def alive_only(*pids):
    return lambda pid: pid in pids


class LostAdoption(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.path = os.path.join(self.tmp, "state.json")

    def test_daemon_save_keeps_an_adoption_another_process_wrote(self):
        daemon = Store(self.path)               # loaded once, kept in memory
        daemon.set_meta("train", json.dumps({"pr-1": {"pr": 1}}))
        cli = Store(self.path)                  # `orchestrator.py adopt 5`
        with cli.tx():
            cli.add_task("pr-5", "daw", "rt-engineer", state="pr_open")
            cli.set_feature("pr-5", pr=5, state="pr_open")
        train = json.loads(cli.get_meta("train"))
        train["pr-5"] = {"pr": 5, "phase": "adotado"}
        cli.set_meta("train", json.dumps(train))
        cli.close()
        daemon.set_meta("tick", "1")            # the daemon's next save
        daemon.close()
        again = Store(self.path)
        self.assertIsNotNone(again.get_feature("pr-5"))
        self.assertIsNotNone(again.get_task("pr-5"))
        self.assertEqual(set(json.loads(again.get_meta("train"))),
                         {"pr-1", "pr-5"})
        self.assertEqual(again.get_meta("tick"), "1")
        self.assertTrue(again.events(kind="state_external_write"))

    def test_own_saves_merge_nothing(self):
        s = Store(self.path)
        s.set_meta("a", "1")
        s.set_meta("b", "2")
        self.assertEqual(s.events(kind="state_external_write"), [])

    def test_what_both_wrote_this_process_wins(self):
        a = Store(self.path)
        b = Store(self.path)
        b.set_meta("k", "theirs")
        a.set_meta("k", "mine")
        self.assertEqual(Store(self.path).get_meta("k"), "mine")


class Lock(unittest.TestCase):
    def setUp(self):
        self.hdir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.hdir, True)

    def test_daemon_holds_cli_is_refused(self):
        daemonlock.acquire(self.hdir, "daemon", pid=11, alive=alive_only(11))
        self.assertTrue(daemonlock.daemon_running(self.hdir, alive_only(11)))
        with self.assertRaises(daemonlock.Busy):
            daemonlock.acquire(self.hdir, "cli", pid=12, alive=alive_only(11, 12))

    def test_a_dead_holder_is_taken_over(self):
        daemonlock.acquire(self.hdir, "daemon", pid=11, alive=alive_only(11))
        daemonlock.acquire(self.hdir, "daemon", pid=13, alive=alive_only(13))
        self.assertEqual(daemonlock.holder(self.hdir, alive_only(13))["pid"], 13)

    def test_daemon_waits_for_a_cli_command_then_starts(self):
        daemonlock.acquire(self.hdir, "cli", pid=12, alive=alive_only(12))
        calls = []

        def sleep(_):   # the CLI command ends while the daemon waits
            calls.append(1)
            daemonlock.release(self.hdir, pid=12)
        daemonlock.acquire(self.hdir, "daemon", wait_s=5, pid=11,
                           alive=alive_only(11, 12), sleep=sleep)
        self.assertEqual(calls, [1])
        self.assertEqual(daemonlock.holder(self.hdir, alive_only(11))["role"],
                         "daemon")

    def test_release_only_removes_our_own(self):
        daemonlock.acquire(self.hdir, "daemon", pid=11, alive=alive_only(11))
        daemonlock.release(self.hdir, pid=99)
        self.assertTrue(os.path.exists(daemonlock.path(self.hdir)))
        daemonlock.release(self.hdir, pid=11)
        self.assertFalse(os.path.exists(daemonlock.path(self.hdir)))


class CliThroughDaemon(support.DaemonCase):
    def cli(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = support.orchestrator.main(["--root", self.tmp] + list(args))
        return rc, out.getvalue()

    def hold_lock_as_daemon(self):
        hdir = os.path.join(self.tmp, ".harness")
        daemonlock.acquire(hdir, "daemon")     # this process: alive
        self.addCleanup(daemonlock.release, hdir)

    def test_adopt_while_the_daemon_runs_is_adopted_by_its_tick(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        self.forge.add_pr(5, "feat/x", ["engine/x.cpp"])
        self.hold_lock_as_daemon()
        before = open(o.store.path, encoding="utf-8").read() \
            if os.path.exists(o.store.path) else None
        rc, out = self.cli("adopt", "5")
        self.assertEqual(rc, 0)
        self.assertIn("queued", out)
        after = open(o.store.path, encoding="utf-8").read() \
            if os.path.exists(o.store.path) else None
        self.assertEqual(before, after)        # the CLI wrote nothing
        o.tick()
        self.assertTrue(o.train.is_adopted("pr-5"))
        o.store.flush()
        self.assertIsNotNone(Store(o.store.path).get_feature("pr-5"))

    def test_add_and_import_queue_go_to_the_mailbox(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        self.hold_lock_as_daemon()
        rc, _ = self.cli("add", "--id", "t1", "--track", "daw", "--agent",
                         "rt-engineer", "--files", "engine/a.cpp")
        self.assertEqual(rc, 0)
        self.assertEqual(o.store.get_task("t1"), None)
        o.tick()
        self.assertEqual(o.store.get_task("t1")["files"], ["engine/a.cpp"])
        self.assertTrue(o.store.events(kind="cli_request"))

    def test_bad_request_is_an_event_not_a_crash(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        cli_mailbox.post(o.hdir, "bundle", name="x", prs=[999])
        o.tick()
        self.assertTrue(o.store.events(kind="cli_request_failed"))
        self.assertEqual(os.listdir(cli_mailbox.request_dir(o.hdir)), [])

    def test_no_daemon_the_cli_writes_under_the_lock_and_releases_it(self):
        rc, _ = self.cli("add", "--id", "t2", "--track", "daw", "--agent",
                         "rt-engineer")
        self.assertEqual(rc, 0)
        self.assertFalse(os.path.exists(
            daemonlock.path(os.path.join(self.tmp, ".harness"))))
        o = self.make()
        self.assertIsNotNone(o.store.get_task("t2"))

    def test_a_second_daemon_refuses_to_start(self):
        # held by another live process (our parent), not by this one
        hdir = os.path.join(self.tmp, ".harness")
        daemonlock.acquire(hdir, "daemon", pid=os.getppid())
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc, _ = self.cli("run", "--once", "--no-broker")
        self.assertEqual(rc, 2)
        self.assertIn("refusing to start", err.getvalue())

    def test_old_daemon_without_lock_is_seen_by_its_status(self):
        odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(odir, exist_ok=True)
        with open(os.path.join(odir, "status.json"), "w") as f:
            f.write("{}")
        rc, out = self.cli("adopt", "7")
        self.assertIn("queued", out)
        self.assertEqual(autoadopt.take_requests(
            os.path.join(self.tmp, ".harness")), [{7}])


if __name__ == "__main__":
    unittest.main()

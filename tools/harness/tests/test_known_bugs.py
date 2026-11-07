"""Orchestrator known bugs (2026-11-06): status.json replace under a
reader, build-slot lock location, git moves in the daemon's checkout. (The
stale-batch case left with the local gate, ADR-0061.)"""
import json
import os
import shutil
import tempfile
import types
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
from harness import gitguard, launcher, scheduler, status


class StatusWrite(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.path = os.path.join(self.tmp, "status.json")

    def test_replace_is_retried_on_permission_error(self):
        real, calls = os.replace, []

        def flaky(a, b):
            calls.append(1)
            if len(calls) < 3:
                raise PermissionError("reader has it open")
            real(a, b)
        with mock.patch.object(os, "replace", flaky), \
                mock.patch.object(status.time, "sleep") as sl:
            status.write_atomic(self.path, {"v": 1})
        self.assertEqual(len(calls), 3)
        self.assertEqual(sl.call_count, 2)
        with open(self.path) as f:
            self.assertEqual(json.load(f), {"v": 1})
        self.assertEqual(os.listdir(self.tmp), ["status.json"])

    def test_gives_up_after_five_tries_and_leaves_no_temp_file(self):
        with mock.patch.object(os, "replace",
                               side_effect=PermissionError("locked")) as rp, \
                mock.patch.object(status.time, "sleep"):
            with self.assertRaises(PermissionError):
                status.write_atomic(self.path, {"v": 1})
        self.assertEqual(rp.call_count, 5)
        self.assertEqual(os.listdir(self.tmp), [])


class BuildSlot(unittest.TestCase):
    LISTING = ("holds pid=77 kind=win user=x prio=0 start=1 "
               "cwd=/c/src/wt/one cmd=bash tools/build-ui.sh \n"
               "waits pid=78 kind=win cwd=/c/src/wt/two cmd=x \n")

    def test_holder_is_found_by_cwd_in_git_bash_form(self):
        self.assertTrue(scheduler.holder_in_list(self.LISTING, "C:\\src\\wt\\one"))

    def test_waiter_counts_as_building_other_worktree_does_not(self):
        # 2026-11-07: a run whose build sat in the lock queue had no stream
        # output, was classified stalled and killed; a waiter is building.
        self.assertTrue(scheduler.holder_in_list(self.LISTING, "C:\\src\\wt\\two"))
        self.assertFalse(scheduler.holder_in_list(self.LISTING, "C:\\src\\wt\\on"))
        self.assertFalse(scheduler.holder_in_list(self.LISTING, "C:\\src\\wt\\three"))

    def test_holds_build_slot_reads_the_script_list(self):
        run = types.SimpleNamespace(stdout=self.LISTING)
        with mock.patch.object(scheduler.subprocess, "run", return_value=run) as r:
            self.assertTrue(scheduler.holds_build_slot("/root", "C:\\src\\wt\\one"))
        self.assertIn("--list", r.call_args[0][0])
        with mock.patch.object(scheduler.subprocess, "run", side_effect=OSError):
            self.assertFalse(scheduler.holds_build_slot("/root", "C:\\src\\wt\\one"))


class DaemonCheckoutGuard(unittest.TestCase):
    ROOT = "C:\\src\\daemon"
    OWN = "C:\\src\\daemon\\.claude\\worktrees\\agent-1"

    def bad(self, cmd, cwd=None):
        return gitguard.violates(cmd, cwd or self.OWN, self.ROOT)

    def test_refuses_movers_with_dash_C_to_the_root(self):
        for sub in ("checkout main", "switch -c x", "reset --hard", "clean -fd",
                    "worktree remove foo"):
            self.assertTrue(self.bad(f"git -C /c/src/daemon {sub}"), sub)
            self.assertTrue(self.bad(f'git -C "C:\\src\\daemon" {sub}'), sub)

    def test_refuses_cd_to_the_root_and_cwd_equal_to_it(self):
        self.assertTrue(self.bad("cd /c/src/daemon && git switch main"))
        self.assertTrue(self.bad("git checkout main", cwd=self.ROOT))
        self.assertTrue(self.bad("echo hi; git reset --hard", cwd=self.ROOT))

    def test_allows_own_worktree_and_harmless_git(self):
        self.assertFalse(self.bad("git checkout -b x"))
        self.assertFalse(self.bad("git -C /c/src/daemon status"))
        self.assertFalse(self.bad("git log", cwd=self.ROOT))
        self.assertFalse(self.bad(f"git -C {self.OWN} clean -fd", cwd=self.ROOT))
        self.assertFalse(self.bad("cd /c/src/daemon && git diff"))

    def test_launcher_registers_the_bash_matcher(self):
        pre = launcher.hook_settings("harness.py")["hooks"]["PreToolUse"]
        self.assertIn("Bash", [m["matcher"] for m in pre])


if __name__ == "__main__":
    unittest.main()

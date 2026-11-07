"""Four orchestrator known bugs (2026-11-06): stale batch of merged PRs,
status.json replace under a reader, build-slot lock location, git moves in
the daemon's checkout."""
import json
import os
import shutil
import tempfile
import types
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
from harness import gitguard, launcher, localci, localci_cancel, scheduler, status
from test_localci import Clock, Store


class StaleBatch(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        forge = types.SimpleNamespace(set_status=lambda *a: None)
        o = types.SimpleNamespace(root=self.tmp, store=Store(), forge=forge,
                                  cfg=types.SimpleNamespace(local_ci_slots=1))
        self.ci = localci.LocalCI(o, Clock())
        self.ci._checkout = lambda wt, heads: (None, [], list(heads))
        self.killed = []
        for target, fn in (("kill_tree", self.killed.append),):
            p = mock.patch.object(localci_cancel, target, fn)
            p.start()
            self.addCleanup(p.stop)
        p = mock.patch.object(localci.subprocess, "Popen",
                              return_value=types.SimpleNamespace(pid=4242))
        p.start()
        self.addCleanup(p.stop)

    def running_batch(self, *heads):
        for h in heads:
            self.ci.start(h)
        self.ci.clock.t += localci.BATCH_SETTLE_S + 1
        self.ci.poll()
        return next(k for k, _ in self.ci._running())

    def test_running_batch_of_merged_prs_is_cancelled_and_killed(self):
        key = self.running_batch("a1", "b2")
        self.ci.prune(lambda h: True)
        self.assertEqual(self.killed, [4242])
        for k in (key, "a1", "b2"):
            self.assertEqual(self.ci.result(k)["state"], "cancelled")
        self.assertEqual(self.ci._running(), [])
        d = self.ci.result(key)["dir"]
        summary = open(os.path.join(d, "summary")).read()
        self.assertFalse(localci.passed(0, summary))
        self.assertFalse(localci.passed(int(open(os.path.join(d, "rc")).read()),
                                        summary))

    def test_batch_with_one_gone_head_is_cancelled_and_the_rest_requeued(self):
        # 2026-11-07: a batch kept building a merged PR's head for ~5 h. Its
        # candidate merges a head nobody will merge, so it is stopped now and
        # the live heads are built again, first, without it.
        key = self.running_batch("a1", "b2")
        self.ci.prune(lambda h: h == "a1")
        self.assertEqual(self.killed, [4242])
        self.assertEqual(self.ci.result(key)["state"], "cancelled")
        self.assertEqual(self.ci.result("a1")["state"], "cancelled")
        self.assertEqual(self.ci._queue("groups"), [["b2"]])
        self.assertEqual(self.ci.result("b2")["state"], "running")
        self.assertTrue(self.ci.result("b2")["queued"])
        self.ci.poll()
        (k2, r2), = self.ci._running()
        self.assertEqual(r2["heads"], ["b2"])

    def test_bisect_group_loses_only_its_gone_heads(self):
        self.ci._set_queue("groups", [["a1", "b2"], ["c3"]])
        self.ci.prune(lambda h: h == "a1")
        self.assertEqual(self.ci._queue("groups"), [["b2"], ["c3"]])

    def test_queued_heads_of_merged_prs_leave_the_queue(self):
        self.ci.start("a1")
        self.ci.start("b2")
        self.ci._set_queue("groups", [["c3", "a1"], ["c3"]])
        self.ci.prune(lambda h: h in ("a1", "c3"))
        self.assertEqual([e["head"] for e in self.ci._queue("pool")], ["b2"])
        self.assertEqual(self.ci._queue("groups"), [])
        self.assertEqual(self.ci.result("a1")["state"], "cancelled")
        self.assertEqual(self.ci.result("b2")["state"], "running")


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

    def test_waiter_and_other_worktree_do_not_hold(self):
        self.assertFalse(scheduler.holder_in_list(self.LISTING, "C:\\src\\wt\\two"))
        self.assertFalse(scheduler.holder_in_list(self.LISTING, "C:\\src\\wt\\on"))

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

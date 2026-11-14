"""The live daemon re-execs itself when origin/main changes its harness
(2026-11-07: it ran a day-old harness). Real git on throwaway repositories;
the re-exec itself is a stub that records the arguments."""
import os
import shutil
import subprocess
import tempfile
import types
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import selfupdate


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t"]
                          + list(args), cwd=cwd, check=True,
                          capture_output=True, text=True).stdout.strip()


def write(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", newline="\n") as f:
        f.write(text)


class Events:
    def __init__(self):
        self.rows, self.flushed = [], 0

    def event(self, *a):
        self.rows.append(a)

    def flush(self):
        self.flushed += 1

    def kinds(self):
        return [r[1] for r in self.rows]


class SelfUpdate(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.origin = os.path.join(self.tmp, "origin.git")
        self.dev = os.path.join(self.tmp, "dev")      # pushes to main
        self.daemon = os.path.join(self.tmp, "daemon")
        subprocess.run(["git", "init", "-q", "--bare", "-b", "main",
                        self.origin], check=True)
        subprocess.run(["git", "clone", "-q", self.origin, self.dev],
                       check=True, capture_output=True)
        write(self.dev, "tools/harness/train.py", "v1\n")
        write(self.dev, "engine/x.cpp", "v1\n")
        git(self.dev, "checkout", "-q", "-b", "main")
        git(self.dev, "add", "-A")
        git(self.dev, "commit", "-qm", "base")
        git(self.dev, "push", "-q", "origin", "main")
        subprocess.run(["git", "clone", "-q", self.origin, self.daemon],
                       check=True, capture_output=True)
        self.store = Events()
        self.running = []
        self.restarted = []
        self.released = []
        self.o = types.SimpleNamespace(
            store=self.store, cfg=types.SimpleNamespace(),
            broker=types.SimpleNamespace(stop=lambda: None))
        self.up = selfupdate.SelfUpdate(
            self.o, ["tools/orchestrator.py", "run", "--live"],
            root=self.daemon, restart=self.restarted.append,
            release=lambda: self.released.append(1))

    def push(self, rel, text):
        write(self.dev, rel, text)
        git(self.dev, "add", "-A")
        git(self.dev, "commit", "-qm", "change " + rel)
        git(self.dev, "push", "-q", "origin", "main")
        return git(self.dev, "rev-parse", "HEAD")

    def test_a_product_change_does_not_restart(self):
        self.push("engine/x.cpp", "v2\n")
        self.assertFalse(self.up.ready(1000))

    def test_a_harness_change_moves_the_checkout_and_restarts(self):
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)
        self.up.restart()
        self.assertEqual(self.store.flushed, 1)
        self.assertEqual(self.restarted[0][1:], ["run", "--live"])
        self.assertTrue(os.path.isabs(self.restarted[0][0]))
        self.assertIn("self_update", self.store.kinds())

    def test_a_dashboard_only_change_moves_the_checkout(self):
        # the dashboard is served from the daemon's checkout (2026-11-08)
        new = self.push("tools/dashboard-static/console-core.js", "v2\n")
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)

    def test_an_agent_definition_change_moves_the_checkout(self):
        # agents are launched from this checkout's .claude/agents (2026-11-08)
        new = self.push(".claude/agents/lead.md", "v2" + chr(10))
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)

    def test_modified_tracked_files_block_the_update(self):
        self.push("tools/harness/train.py", "v2\n")
        write(self.daemon, "engine/x.cpp", "local edit\n")
        self.assertFalse(self.up.ready(1000))
        self.assertIn("self_update_blocked", self.store.kinds())
        self.assertEqual(self.restarted, [])

    def test_checks_at_most_every_check_s(self):
        self.assertFalse(self.up.ready(1000))
        self.push("tools/build-slot.sh", "v2\n")
        self.assertFalse(self.up.ready(1000 + 10))
        self.assertTrue(self.up.ready(1000 + selfupdate.CHECK_S))

    def test_a_dirty_checkout_is_forced_after_the_bound(self):
        new = self.push("tools/harness/train.py", "v2\n")
        write(self.daemon, "engine/x.cpp", "local edit\n")
        self.assertFalse(self.up.ready(1000))
        self.assertFalse(self.up.ready(1000 + self.up.max_defer_s - 1))
        self.assertTrue(self.up.ready(1000 + self.up.max_defer_s))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)
        self.assertIn("self_update_forced", self.store.kinds())

    def test_a_defer_reason_holds_back_only_until_the_bound(self):
        self.up.defer_reason = lambda: "integrator running"
        self.push("tools/harness/train.py", "v2\n")
        self.assertFalse(self.up.ready(1000))
        self.assertFalse(self.up.ready(1000 + 100))
        self.assertTrue(self.up.ready(1000 + self.up.max_defer_s))

    def test_local_plan_commits_are_rebased_not_dropped(self):
        # 2026-11-08: an architect committed plans in the daemon checkout.
        write(self.daemon, "planning/plans/x.md", "plan\n")
        git(self.daemon, "add", "-A")
        git(self.daemon, "commit", "-qm", "x: plan")
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD~1"), new)
        self.assertTrue(os.path.exists(
            os.path.join(self.daemon, "planning/plans/x.md")))
        self.assertIn("self_update_rebased", self.store.kinds())

    def test_local_commit_outside_plans_is_alerted_not_rebased(self):
        write(self.daemon, "engine/y.cpp", "local\n")
        git(self.daemon, "add", "-A")
        git(self.daemon, "commit", "-qm", "stray product edit")
        self.push("tools/harness/train.py", "v2\n")
        self.assertFalse(self.up.ready(1000))
        ev = [r for r in self.store.rows if r[1] == "self_update_blocked"]
        self.assertIn("ALERT", ev[0][2])
        self.assertIn("engine/y.cpp", ev[0][2])
        self.assertNotIn("self_update_rebased", self.store.kinds())

    def test_a_conflicting_local_commit_blocks_and_reports(self):
        write(self.daemon, "tools/harness/train.py", "local\n")
        git(self.daemon, "add", "-A")
        git(self.daemon, "commit", "-qm", "local harness edit")
        self.push("tools/harness/train.py", "v2\n")
        self.assertFalse(self.up.ready(1000))
        self.assertIn("self_update_blocked", self.store.kinds())
        self.assertEqual(git(self.daemon, "status", "--porcelain"), "")

    def commit_plan(self, rel, text):
        write(self.daemon, rel, text)
        git(self.daemon, "add", "-A")
        git(self.daemon, "commit", "-qm", "plan " + rel)

    def test_a_plan_also_merged_on_main_rebases_with_main_winning(self):
        # 2026-11-09: a feature PR carried its plan and ADR to main with
        # edits; the planner's local commit of the same files conflicted, the
        # rebase was aborted every 5 min and the daemon ran 11 commits behind
        # main for hours (#607 never loaded; plugins.offline stalled).
        self.commit_plan("brain/decisions/ADR-1.md", "draft\n")
        self.commit_plan("planning/plans/y.md", "local only\n")
        self.push("brain/decisions/ADR-1.md", "reviewed\n")
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "merge-base", "HEAD", new), new)
        with open(os.path.join(self.daemon, "brain/decisions/ADR-1.md")) as f:
            self.assertEqual(f.read(), "reviewed\n")
        self.assertTrue(os.path.exists(
            os.path.join(self.daemon, "planning/plans/y.md")))
        self.assertEqual(git(self.daemon, "status", "--porcelain"), "")
        ev = [r for r in self.store.rows if r[1] == "self_update_rebased"]
        self.assertIn("main's version", ev[0][2])

    def test_an_unrebaseable_plan_commit_is_saved_and_forced_at_the_bound(self):
        # modify/delete: no merge option resolves it, yet the daemon must
        # still reach main's harness within the bound, keeping the commits.
        self.commit_plan("planning/plans/z.md", "v1\n")
        git(self.dev, "pull", "-q", self.daemon, "HEAD")
        git(self.dev, "push", "-q", "origin", "main")
        self.commit_plan("planning/plans/z.md", "local v2\n")
        git(self.dev, "rm", "-q", "planning/plans/z.md")
        git(self.dev, "commit", "-qm", "drop z")
        git(self.dev, "push", "-q", "origin", "main")
        local = git(self.daemon, "rev-parse", "HEAD")
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertFalse(self.up.ready(1000))
        self.assertIn("self_update_blocked", self.store.kinds())
        self.assertEqual(git(self.daemon, "status", "--porcelain"), "")
        self.assertTrue(self.up.ready(1000 + self.up.max_defer_s))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)
        ev = [r for r in self.store.rows if r[1] == "self_update_forced"]
        saved = ev[-1][2].split()[-1]
        self.assertEqual(git(self.daemon, "rev-parse", saved), local)

    def test_a_foreign_commit_is_kept_on_a_branch_and_dropped_at_the_bound(self):
        # 2026-11-12/13: a product commit and a tracked paused.json in the
        # daemon checkout blocked the update ~8 h ("move them to a branch",
        # forever). Past the bound the harness keeps them on a branch, goes
        # to main and replays the plan commits; the owner's pause survives.
        self.commit_plan("planning/plans/p.md", "plan\n")
        write(self.daemon, "plugins/amp/tests/t.cpp", "stray\n")
        write(self.daemon, "planning/orchestrator/paused.json", '["qa"]')
        git(self.daemon, "add", "-A")
        git(self.daemon, "commit", "-qm", "stray product edit + runtime")
        self.commit_plan("brain/decisions/ADR-9.md", "adr\n")
        local = git(self.daemon, "rev-parse", "HEAD")
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertFalse(self.up.ready(1000))
        self.assertFalse(self.up.ready(1000 + self.up.max_defer_s - 1))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), local)
        self.assertTrue(self.up.ready(1000 + self.up.max_defer_s))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD~2"), new)
        for rel in ("planning/plans/p.md", "brain/decisions/ADR-9.md"):
            self.assertTrue(os.path.exists(os.path.join(self.daemon, rel)))
        self.assertFalse(os.path.exists(
            os.path.join(self.daemon, "plugins/amp/tests/t.cpp")))
        with open(os.path.join(self.daemon,
                               "planning/orchestrator/paused.json")) as f:
            self.assertEqual(f.read(), '["qa"]')
        ev = [r for r in self.store.rows if r[1] == "self_update_forced"]
        self.assertEqual(len(ev), 1)
        self.assertIn("ALERT", ev[0][2])
        self.assertIn("plugins/amp/tests/t.cpp", ev[0][2])
        saved = ev[0][2].split()[-1]
        self.assertTrue(saved.startswith("keep/daemon-"))
        self.assertEqual(git(self.daemon, "rev-parse", saved), local)

    def test_an_untracked_file_main_carries_is_moved_aside(self):
        # 2026-11-13: a planner's uncommitted ADR, now on main, made git
        # refuse the move ("untracked working tree files would be
        # overwritten"). It goes under .harness/ and the update proceeds.
        self.push(".gitignore", ".harness/\n")
        git(self.daemon, "pull", "-q", "origin", "main")
        write(self.daemon, "brain/decisions/ADR-7.md", "draft\n")
        write(self.daemon, "brain/notes.md", "mine\n")
        self.push("brain/decisions/ADR-7.md", "merged\n")
        new = self.push("tools/harness/train.py", "v2\n")
        self.assertTrue(self.up.ready(1000))
        self.assertEqual(git(self.daemon, "rev-parse", "HEAD"), new)
        with open(os.path.join(self.daemon, "brain/decisions/ADR-7.md")) as f:
            self.assertEqual(f.read(), "merged\n")
        self.assertTrue(os.path.exists(
            os.path.join(self.daemon, "brain/notes.md")))  # not in the way
        ev = [r for r in self.store.rows if r[1] == "self_update_aside"]
        self.assertEqual(len(ev), 1)
        dest = ev[0][2].split(" moved to ")[1].split(": ")[0]
        self.assertTrue(dest.startswith(os.path.join(self.daemon,
                                                     ".harness")))
        with open(os.path.join(dest, "brain/decisions/ADR-7.md")) as f:
            self.assertEqual(f.read(), "draft\n")

    def test_paused_json_is_ignored_runtime_state(self):
        # 2026-11-12: a planner's `git add` committed it in a plan commit.
        # The repository's .gitignore alone, in a bare repo of its own, so
        # a local info/exclude cannot make this pass.
        shutil.copy(os.path.join(selfupdate.CODE_ROOT, ".gitignore"),
                    os.path.join(self.dev, ".gitignore"))
        r = subprocess.run(["git", "check-ignore", "-q",
                            "planning/orchestrator/paused.json"],
                           cwd=self.dev)
        self.assertEqual(r.returncode, 0)

    def test_an_error_in_the_check_is_one_event_not_silence(self):
        def boom(now):
            raise RuntimeError("git exploded")
        self.up.ready = boom
        self.assertFalse(self.up.ready_or_report(1000))
        self.assertFalse(self.up.ready_or_report(1001))
        self.assertEqual(self.store.kinds().count("self_update_error"), 1)

    def test_event_names_old_and_new_sha(self):
        old = git(self.daemon, "rev-parse", "HEAD")
        new = self.push("tools/harness/train.py", "v2\n")
        self.up.ready(1000)
        ev = [r for r in self.store.rows if r[1] == "self_update"][0]
        self.assertIn(f"{old[:12]} -> {new[:12]}", ev[2])


class DaemonLoop(support.DaemonCase):
    def test_run_loop_restarts_when_the_updater_is_ready(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        calls = []
        o.updater = types.SimpleNamespace(ready_or_report=lambda now: True,
                                          restart=lambda: calls.append(1))
        o.run(max_ticks=5, sleep=lambda s: None)
        self.assertEqual(calls, [1])


class ReAttach(support.DaemonCase):
    def test_runs_alive_across_a_restart_are_adopted_not_relaunched(self):
        o = self.make(floors={"daw": 0}, dry_run=True)
        import harness.launcher as launcher
        runs = [{"id": "r1", "name": "a1", "pid": 4242, "task_id": "t1"},
                {"id": "r2", "name": "a2", "pid": 4343, "task_id": "t2"}]
        from unittest import mock
        seen = []
        o.broker.expect = seen.append
        with mock.patch.object(o.store, "active_runs", lambda: runs),                 mock.patch.object(o.store, "set_run", lambda *a, **k: None),                 mock.patch.object(launcher, "pid_alive",
                                  lambda pid: pid == 4242):
            self.assertEqual(o.adopt_runs(), ["r1"])
        self.assertEqual(seen, ["a1"])


if __name__ == "__main__":
    unittest.main()

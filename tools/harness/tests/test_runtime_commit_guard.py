"""Nothing the harness or a planner commits carries the daemon's runtime
files (2026-11-14: a plan commit in the daemon checkout tracked state.json,
events.jsonl and .harness/plan/*, and the next forced self-update deleted
them). Real git for the staging paths, a fake git for the Bash guard."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import commitguard, heartbeat, runtime_files, selfupdate
from test_self_update import git, write

STATE = "planning/orchestrator/state.json"
RUNTIME_SAMPLES = (STATE, "planning/orchestrator/events.jsonl",
                   "planning/orchestrator/events.3.jsonl",
                   "planning/orchestrator/status.json",
                   "planning/orchestrator/state.json.bak.2",
                   "planning/orchestrator/paused.json",
                   "planning/orchestrator/plans/x.plan.json",
                   ".harness/plan/x.plan.json")


class Lists(unittest.TestCase):
    def test_runtime_paths(self):
        for p in RUNTIME_SAMPLES + (".\\.harness\\ci\\x",):
            self.assertTrue(runtime_files.is_runtime(p), p)
        for p in ("planning/plans/x.md", "planning/orchestrator/"
                  "orchestrator.toml", "brain/decisions/ADR-1.md",
                  "tools/harness/store.py", "planning/orchestrator/"
                  "plan.schema.json"):
            self.assertFalse(runtime_files.is_runtime(p), p)

    def test_gitignore_ignores_every_runtime_path(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        subprocess.run(["git", "init", "-q", tmp], check=True)
        shutil.copy(os.path.join(selfupdate.CODE_ROOT, ".gitignore"),
                    os.path.join(tmp, ".gitignore"))
        for p in RUNTIME_SAMPLES:
            r = subprocess.run(["git", "check-ignore", "-q", p], cwd=tmp)
            self.assertEqual(r.returncode, 0, p)


class Staging(unittest.TestCase):
    """A checkout whose HEAD already tracks state.json (the bad commit)."""

    def setUp(self):
        self.repo = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.repo, True)
        git(self.repo, "init", "-q")
        write(self.repo, "planning/plans/a.md", "a\n")
        write(self.repo, STATE, "{}")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-qm", "bad: tracks state.json")
        write(self.repo, STATE, '{"changed": 1}')
        write(self.repo, ".harness/plan/x.json", "{}")
        write(self.repo, "planning/plans/b.md", "b\n")

    def run_git(self, args):
        return git(self.repo, *args[1:])

    def committed(self):
        return git(self.repo, "show", "--name-only", "--format=", "HEAD")

    def test_stage_all_leaves_runtime_files_out(self):
        runtime_files.stage_all(self.run_git)
        staged = git(self.repo, "diff", "--cached", "--name-only")
        self.assertEqual(staged.splitlines(), ["planning/plans/b.md"])

    def test_staged_runtime_files_are_unstaged(self):
        git(self.repo, "add", "-f", STATE, ".harness/plan/x.json")
        self.assertEqual(runtime_files.unstage(self.run_git),
                         [".harness/plan/x.json", STATE])
        self.assertEqual(git(self.repo, "diff", "--cached", "--name-only"),
                         "")
        with open(os.path.join(self.repo, STATE)) as f:
            self.assertEqual(f.read(), '{"changed": 1}')   # still on disk

    def test_the_wip_checkpoint_never_commits_runtime_files(self):
        self.assertTrue(heartbeat.wip_checkpoint(self.repo))
        self.assertEqual(self.committed(), "planning/plans/b.md")


class FakeGit:
    def __init__(self, staged="", tree="", unstaged=""):
        self.out = {"--cached": staged, "ls-tree": tree, "diff": unstaged}

    def __call__(self, where, args):
        if args[0] == "ls-tree":
            return self.out["ls-tree"]
        return self.out["--cached"] if "--cached" in args else \
            self.out["diff"]


class BashGuard(unittest.TestCase):
    ROOT = "C:\\src\\daemon"
    OWN = "C:\\src\\daemon\\.claude\\worktrees\\agent-1"

    def why(self, cmd, cwd=None, **git):
        return commitguard.reason(cmd, cwd or self.ROOT, self.ROOT,
                                  FakeGit(**git))

    def test_forced_add_in_the_daemon_checkout_is_refused(self):
        # what the architect ran to commit e21d3038
        self.assertTrue(self.why("git add -f planning/ .harness/plan"))
        self.assertTrue(self.why("git add --force planning/plans/x.md"))
        self.assertTrue(self.why("git add -fA"))

    def test_naming_a_runtime_file_is_refused(self):
        self.assertTrue(self.why(f"git add {STATE}"))
        self.assertTrue(self.why("cd planning/orchestrator && git add "
                                 "events.jsonl"))

    def test_broad_add_is_refused_only_while_head_tracks_runtime(self):
        self.assertIsNone(self.why("git add -A"))
        self.assertTrue(self.why("git add -A", tree=STATE + "\n"))
        self.assertTrue(self.why("git add .", tree=STATE + "\n"))
        self.assertIsNone(self.why("git add planning/plans/x.md",
                                   tree=STATE + "\n"))

    def test_commit_refused_while_runtime_files_are_staged(self):
        self.assertTrue(self.why("git commit -m plan", staged=STATE + "\n"))
        self.assertTrue(self.why("git commit -am plan", unstaged=STATE))
        self.assertIsNone(self.why("git commit -m plan",
                                   staged="planning/plans/x.md\n"))

    def test_plans_by_explicit_path_and_own_worktree_pass(self):
        self.assertIsNone(self.why(
            "git add planning/plans/x.md brain/decisions/ADR-1.md && "
            "git commit -m 'x: plan'"))
        self.assertIsNone(self.why("git add -f build/x", cwd=self.OWN,
                                   staged=STATE))


class HookEndToEnd(unittest.TestCase):
    """The PreToolUse hook as Claude Code runs it, cwd the daemon root."""

    def hook(self, command):
        env = dict(os.environ, SONORA_HARNESS_PORT="1",
                   SONORA_AGENT_NAME="architect", SONORA_HARNESS_TOKEN="t")
        data = {"tool_name": "Bash", "cwd": selfupdate.CODE_ROOT,
                "tool_input": {"command": command}}
        return subprocess.run(
            [sys.executable, os.path.join(selfupdate.CODE_ROOT, "tools",
                                          "harness.py"), "check", "--hook"],
            input=json.dumps(data), capture_output=True, text=True, env=env,
            timeout=60)

    def test_forced_add_is_denied_and_a_plain_one_allowed(self):
        r = self.hook("git add -f planning/orchestrator/state.json")
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("runtime files", r.stderr)
        self.assertEqual(self.hook("git status").returncode, 0)


if __name__ == "__main__":
    unittest.main()

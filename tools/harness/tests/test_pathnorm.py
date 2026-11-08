"""Hook path vs claim path in nested worktrees (2026-11-07, PR #501 fixer)."""
import io
import json
import os
import shutil
import tempfile
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
from harness import pathnorm


def _load_cli():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "harness_cli", os.path.join(support.TOOLS, "harness.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Candidates(unittest.TestCase):
    OUTER = "C:/src/main"
    INNER = OUTER + "/.claude/worktrees/feat"

    def exists(self, p):
        return p in (self.OUTER + "/.git", self.INNER + "/.git")

    def test_nested_worktree_yields_inner_then_outer_relative(self):
        c = pathnorm.candidates(self.INNER + "/tools/module-deps.allow",
                                self.exists)
        self.assertEqual(c[0], "tools/module-deps.allow")
        self.assertIn(".claude/worktrees/feat/tools/module-deps.allow", c)
        self.assertEqual(c[-1], self.INNER + "/tools/module-deps.allow")

    def test_windows_backslashes_match_posix(self):
        win = self.INNER.replace("/", "\\") + "\\tools\\x.txt"
        self.assertEqual(pathnorm.candidates(win, self.exists),
                         pathnorm.candidates(self.INNER + "/tools/x.txt",
                                             self.exists))

    def test_no_repo_keeps_plain_path(self):
        self.assertEqual(pathnorm.candidates("/tmp/a", lambda p: False),
                         ["/tmp/a"])


class HookEndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        inner = os.path.join(self.tmp, ".claude", "worktrees", "feat")
        os.makedirs(os.path.join(inner, "tools"))
        for r in (self.tmp, inner):
            open(os.path.join(r, ".git"), "w").close()
        self.path = os.path.join(inner, "tools", "module-deps.allow")
        self.cli = _load_cli()

    def run_hook(self, claimed):
        asked = []

        def oneshot(port, name, token, op, **f):
            asked.append(f["path"])
            return {"ok": True, "allowed": f["path"] in claimed}
        data = {"tool_name": "Edit", "tool_input": {"file_path": self.path}}
        cfg = {"port": 1, "name": "n", "token": "t"}
        with mock.patch.object(self.cli, "oneshot", oneshot), \
                mock.patch.object(self.cli, "env_config", lambda: cfg), \
                mock.patch.object(self.cli, "hook_input", lambda: data), \
                mock.patch.dict(os.environ, {"SONORA_UNCLAIMED_OK": "0"}):
            return self.cli.run_hook(mock.Mock(), False), asked

    def test_relative_claim_allows_absolute_edit(self):
        self.assertEqual(self.run_hook({"tools/module-deps.allow"})[0], 0)

    def test_outer_relative_claim_allows_edit(self):
        claim = ".claude/worktrees/feat/tools/module-deps.allow"
        self.assertEqual(self.run_hook({claim})[0], 0)

    def test_unclaimed_still_denied(self):
        with mock.patch("sys.stderr", io.StringIO()):
            self.assertEqual(self.run_hook(set())[0], 2)


if __name__ == "__main__":
    unittest.main()

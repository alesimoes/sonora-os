import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from harness import agent_perms, launcher  # noqa: E402


class AgentClaudePermissions(unittest.TestCase):
    def test_only_context_and_rules_are_opened(self):
        rules = agent_perms.writable_rules()
        self.assertEqual(len(rules), 6)
        for r in rules:
            self.assertTrue(r.endswith("(/.claude/context/**)")
                            or r.endswith("(/.claude/rules/**)"), r)
        for r in rules:
            for bad in ("agents", "settings", "hooks", "skills", "routing"):
                self.assertNotIn(bad, r)

    def test_launch_command_carries_the_rules(self):
        argv = launcher.build_command(["claude"], "x", "p", "m", "s")
        allowed = argv[argv.index("--allowedTools") + 1].split(",")
        for r in agent_perms.writable_rules():
            self.assertIn(r, allowed)
        self.assertIn("Edit", allowed)
        self.assertNotIn("Edit(/.claude/**)", allowed)

    def test_run_settings_file_carries_the_rules(self):
        with tempfile.TemporaryDirectory() as d:
            _, settings = launcher.write_run_files(d, {})
            with open(settings, encoding="utf-8") as f:
                cfg = json.load(f)
        self.assertEqual(cfg["permissions"]["allow"],
                         agent_perms.writable_rules())
        self.assertIn("PreToolUse", cfg["hooks"])


if __name__ == "__main__":
    unittest.main()

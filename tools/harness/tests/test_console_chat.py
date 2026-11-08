"""The console's read side: the merged conversation (harness/console_chat.py)
and the agent's reading helper (tools/console_read.py)."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import console_actions as A, console_chat, console_store as S

TOOLS = os.path.join(os.path.dirname(__file__), "..", "..")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)


class View(Base):
    def test_agent_answers_appear_after_the_owner_wrote_to_that_agent(self):
        S.add_message(self.tmp, "user", "muda para B", to="eng-1")
        os.makedirs(os.path.join(self.tmp, "log"))
        rows = [{"id": "1", "ts": "2000-01-01T00:00:00.000Z", "from": "eng-1", "to": "daemon",
                 "type": "note", "body": {"text": "old, before the owner wrote"}},
                {"id": "2", "ts": "2999-01-01T00:00:00.000Z", "from": "eng-1", "to": "daemon",
                 "type": "answer", "body": {"text": "recebi"}},
                {"id": "3", "ts": "2999-01-01T00:00:00.000Z", "from": "eng-2", "to": "daemon",
                 "type": "answer", "body": {"text": "not asked"}}]
        with open(os.path.join(self.tmp, "log", "messages.jsonl"), "w") as f:
            f.write("\n".join(json.dumps(r) for r in rows) + "\n")
        msgs = console_chat.view(self.tmp)["messages"]
        self.assertEqual([(m["kind"], m["text"]) for m in msgs], [("user", "muda para B"), ("agent", "recebi")])
        self.assertEqual(msgs[1]["from"], "eng-1")

    def test_card_states_travel_with_the_messages_and_expire_on_read(self):
        rec = A.propose(self.tmp, "drain", {}, 1000.0)
        S.add_message(self.tmp, "orch", "ok", cards=[{"type": "action", "id": rec["id"]}])
        recs = S.load_actions(self.tmp)
        recs[rec["id"]]["created_ts"] = 1.0      # long ago
        S.save_actions(self.tmp, recs)
        v = console_chat.view(self.tmp, now=1e9)
        self.assertEqual(v["actions"][rec["id"]]["state"], "expired")
        self.assertNotIn("args", v["actions"][rec["id"]])

    def test_only_the_last_200_messages(self):
        for i in range(S.HISTORY + 5):
            S.add_message(self.tmp, "system", str(i))
        self.assertEqual(len(console_chat.view(self.tmp)["messages"]), S.HISTORY)


class ReadHelper(Base):
    def run_read(self, *args):
        env = dict(os.environ, SONORA_HARNESS=self.tmp)
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "console_read.py"), *args],
                           capture_output=True, text=True, env=env, encoding="utf-8")
        return r.returncode, r.stdout

    def test_inbox_lists_only_unanswered_messages(self):
        a = S.add_message(self.tmp, "user", "primeira")
        S.add_message(self.tmp, "orch", "resp", req=a["id"])
        b = S.add_message(self.tmp, "user", "segunda")
        code, out = self.run_read("inbox")
        self.assertEqual(code, 0)
        self.assertIn(b["id"] + ": segunda", out)
        self.assertNotIn(a["id"] + ":", out)

    def test_unknown_command_prints_usage(self):
        code, out = self.run_read("rm")
        self.assertEqual(code, 2)
        self.assertIn("inbox", out)

    def test_why_runs_without_a_daemon(self):
        code, out = self.run_read("why", "#544")
        self.assertEqual(code, 0)
        self.assertIn("PR #544", out)


if __name__ == "__main__":
    unittest.main()

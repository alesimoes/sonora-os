"""The console's request door (harness/console_door.py + ownerchat.tick):
what the dashboard may post, and what the daemon makes of each request."""
import json
import os
import unittest

import support
from harness import console_door, console_store as S, ownerchat


class Post(unittest.TestCase):
    def setUp(self):
        import tempfile
        import shutil
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def files(self):
        d = ownerchat.request_dir(self.tmp)
        out = []
        for n in sorted(os.listdir(d)):
            with open(os.path.join(d, n), encoding="utf-8") as f:
                out.append(json.load(f))
        return out

    def test_a_message_becomes_a_request_file_at_the_ownerchat_door(self):
        console_door.post(self.tmp, "console", text="  status  ", to=None)
        [f] = self.files()
        self.assertEqual(f.pop("sent")[:2], "20")
        self.assertEqual(f, {"kind": "console", "text": "status", "to": None})

    def test_bad_requests_are_refused_before_they_are_written(self):
        bad = [("console", {"text": "  "}), ("console", {"text": "x", "to": "../etc"}),
               ("confirm", {"action_id": "../x"}), ("confirm", {}),
               ("run", {"text": "x"}), ("confirm", {"action_id": "a12x"})]
        for kind, f in bad:
            with self.assertRaises(ValueError, msg=(kind, f)):
                console_door.post(self.tmp, kind, **f)
        self.assertFalse(os.path.isdir(ownerchat.request_dir(self.tmp)))

    def test_a_repeat_waiting_at_the_door_is_absorbed(self):
        a = console_door.post(self.tmp, "console", text="por que o PR 599?")
        b = console_door.post(self.tmp, "console", text="por que o PR 599?")
        console_door.post(self.tmp, "console", text="por que o PR 599?", to="eng-1")
        self.assertEqual(a, b)
        self.assertEqual(len(self.files()), 2)

    def test_long_text_is_cut(self):
        console_door.post(self.tmp, "console", text="x" * 5000)
        self.assertEqual(len(self.files()[0]["text"]), console_door.MAX_TEXT)


class InTheTick(support.DaemonCase):
    def orch(self):
        o = self.make(floors={"daw": 0})
        o.store.add_task("t1", "daw", "rt-engineer")
        o.store.add_run(id="r1", task_id="t1", role="engineer", name="eng-1",
                        started=1.0, ended=2.0)
        return o

    def test_a_message_for_the_orchestrator_is_stored_and_waits_for_the_run(self):
        o = self.orch()
        console_door.post(o.hdir, "console", text="como está tudo?")
        ownerchat.tick(o)
        got = S.history(o.hdir)
        self.assertEqual([(m["kind"], m["text"]) for m in got], [("user", "como está tudo?")])
        self.assertEqual(len(S.unanswered(o.hdir)), 1)
        self.assertEqual(o.broker.sent, [])
        self.assertEqual(os.listdir(ownerchat.request_dir(o.hdir)), [])

    def test_a_message_for_an_agent_goes_through_ownerchat_ask(self):
        o = self.orch()
        console_door.post(o.hdir, "console", text="muda para a rota B", to="eng-1")
        ownerchat.tick(o)
        sent = [(s[0], s[1], s[2]["text"]) for s in o.broker.sent]
        self.assertEqual(sent, [("eng-1", "owner_ask", "muda para a rota B")])
        self.assertEqual(S.history(o.hdir)[0]["to"], "eng-1")
        self.assertEqual(S.unanswered(o.hdir), [])   # the agent answers, not the console run

    def test_an_unknown_agent_is_said_so_in_the_chat(self):
        o = self.orch()
        console_door.post(o.hdir, "console", text="oi", to="ghost-9")
        ownerchat.tick(o)
        self.assertEqual(o.broker.sent, [])
        self.assertIn("ghost-9", S.history(o.hdir)[-1]["text"])

    def test_a_refused_console_request_does_not_stop_the_other_requests(self):
        o = self.orch()
        console_door.post(o.hdir, "confirm", action_id="a000")      # unknown card
        ownerchat.write_request(o.hdir, "ask", "eng-1", "still delivered")
        ownerchat.tick(o)
        self.assertIn("owner_ask", [s[1] for s in o.broker.sent])


if __name__ == "__main__":
    unittest.main()

"""The console's whitelist (harness/console_verbs.py, console_actions.propose):
only listed verbs, only clean arguments, wording written by the daemon."""
import json
import os
import unittest

import support
from harness import console, console_actions as A, console_store as S
from harness.console_verbs import VERBS


class Check(unittest.TestCase):
    def test_only_the_whitelisted_verbs_exist(self):
        self.assertEqual(sorted(VERBS), sorted([
            "pool.pause", "pool.resume", "drain", "stop", "resume", "task.add", "pr.adopt",
            "agent.ask", "agent.wake", "run.stop", "task.requeue", "task.close"]))

    def test_unknown_verbs_and_extra_arguments_are_refused(self):
        for verb, args in [("rm.rf", {}), ("shell", {"cmd": "ls"}), ("stop", {"now": True}),
                           ("pool.pause", {"pool": "x", "extra": 1}), ("run.stop", {})]:
            with self.assertRaises(ValueError, msg=verb):
                A.check(verb, args)

    def test_arguments_are_validated(self):
        bad = [("pool.pause", {"pool": "a b"}), ("pool.pause", {"pool": "../x"}),
               ("pr.adopt", {"prs": []}), ("pr.adopt", {"prs": [True]}),
               ("pr.adopt", {"prs": list(range(1, 20))}), ("agent.ask", {"name": "eng-1", "text": " "}),
               ("task.add", {"id": "x", "track": "daw"}), ("task.close", {"task": "a;b"})]
        for verb, args in bad:
            with self.assertRaises(ValueError, msg=(verb, args)):
                A.check(verb, args)

    def test_the_card_text_comes_from_the_daemon_not_the_model(self):
        clean, card = A.check("run.stop", {"name": "eng-104"})
        self.assertEqual(clean, {"name": "eng-104"})
        self.assertIn("eng-104", card.title)
        self.assertTrue(card.steps and card.effect)

    def test_risky_verbs_are_marked(self):
        risky = {v for v, x in VERBS.items() if x.risk}
        self.assertEqual(risky, {"drain", "stop", "run.stop", "task.close"})


class Propose(support.DaemonCase):
    def test_a_proposal_is_pending_and_logged_and_runs_nothing(self):
        o = self.make()
        rec = A.propose(o.hdir, "stop", {}, o.clock())
        self.assertEqual(rec["state"], "pending")
        self.assertFalse(os.path.exists(os.path.join(o.odir, "STOP")))
        self.assertEqual(S.audit_tail(o.hdir)[-1]["event"], "proposed")

    def test_a_newer_card_on_the_same_target_replaces_the_older_one(self):
        o = self.make()
        old = A.propose(o.hdir, "run.stop", {"name": "eng-1"}, o.clock())
        other = A.propose(o.hdir, "run.stop", {"name": "eng-2"}, o.clock())
        new = A.propose(o.hdir, "run.stop", {"name": "eng-1"}, o.clock())
        acts = S.load_actions(o.hdir)
        self.assertEqual([acts[i]["state"] for i in (old["id"], other["id"], new["id"])],
                         ["cancelled", "pending", "pending"])


class Reply(support.DaemonCase):
    def reply(self, o, body, mid="msg1"):
        S.add_message(o.hdir, "user", "pare o eng-1")
        console.on_reply(o, {"id": mid, "from": "console", "type": "console_reply", "body": body})
        return S.history(o.hdir)[-1]

    def test_a_reply_turns_listed_actions_into_cards_and_refuses_the_rest(self):
        o = self.make()
        m = self.reply(o, {"text": "Posso parar.", "actions": [
            {"verb": "run.stop", "args": {"name": "eng-1"}},
            {"verb": "rm", "args": {"path": "/"}}]})
        self.assertEqual(m["kind"], "orch")
        self.assertEqual(len(m["cards"]), 1)
        self.assertIn("Recusei", m["text"])
        self.assertEqual(len(S.load_actions(o.hdir)), 1)

    def test_at_most_three_actions_and_a_list_card_is_cleaned(self):
        o = self.make()
        acts = [{"verb": "pool.pause", "args": {"pool": f"p{i}"}} for i in range(6)]
        m = self.reply(o, {"text": "t", "actions": acts,
                           "list": {"title": "L", "rows": ["a", {"text": "b", "id": "eng-1"}, 7, {}]}})
        kinds = [c["type"] for c in m["cards"]]
        self.assertEqual(kinds, ["list", "action", "action", "action"])
        self.assertEqual(m["cards"][0]["rows"], [{"text": "a"}, {"text": "b", "id": "eng-1"}])

    def test_the_same_reply_twice_is_stored_once(self):
        o = self.make()
        body = {"text": "oi"}
        self.reply(o, body, "same")
        console.on_reply(o, {"id": "same", "body": body})
        self.assertEqual(len([m for m in S.history(o.hdir) if m["kind"] == "orch"]), 1)

    def test_a_reply_answers_the_message_even_with_a_wrong_req(self):
        o = self.make()
        S.add_message(o.hdir, "user", "oi")
        console.on_reply(o, {"id": "r", "body": {"text": "ola", "req": "nonsense"}})
        self.assertEqual(S.unanswered(o.hdir), [])

    def test_a_bad_body_is_ignored(self):
        o = self.make()
        console.on_reply(o, {"id": "r", "body": "text"})
        self.assertEqual(S.history(o.hdir), [])

    def test_the_message_type_is_on_the_wire(self):
        from harness import protocol as P
        self.assertIn("console_reply", P.MSG_TYPES)
        self.assertEqual(json.loads(json.dumps(sorted(P.MSG_TYPES))).count("console_reply"), 1)


if __name__ == "__main__":
    unittest.main()

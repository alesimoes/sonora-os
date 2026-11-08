"""One console run kept alive between messages (console_session) and the
half-written answer (partial replies)."""
import unittest

import support
from harness import console, console_chat, console_door, console_session as session
from harness import console_store as S, ownerchat


class Session(support.DaemonCase):
    def ask(self, o, text="oi"):
        console_door.post(o.hdir, "console", text=text)
        ownerchat.tick(o)

    def wakes(self, o):
        return [w for w in o.store.tasks(kind="wake")
                if w["agent"] == "console" and w["state"] == "queued"]

    def live_run(self, o):
        o.store.add_task("w", None, "console", kind="wake")
        o.store.set_task("w", state="merged")
        o.store.add_run(id="rc", task_id="w", role="planner", name="console", started=1.0)

    def to_console(self, o):
        return [s for s in o.broker.sent if s[0] == "console"]

    def test_a_second_message_goes_to_the_live_run_and_starts_no_new_one(self):
        o = self.make()
        self.ask(o, "um")
        console.tick(o, o.clock())               # launches the run; "um" is in its inbox
        self.assertEqual(len(self.wakes(o)), 1)
        self.live_run(o)
        console.tick(o, o.clock())
        self.assertEqual(self.to_console(o), [])  # the inbox already had it
        self.ask(o, "dois")
        console.tick(o, o.clock())
        sent = self.to_console(o)
        self.assertEqual([(s[1], s[2]["text"]) for s in sent], [("owner_ask", "dois")])
        self.assertEqual(sent[0][2]["req"], S.unanswered(o.hdir)[-1]["id"])
        console.tick(o, o.clock())               # not sent twice
        self.assertEqual(len(self.to_console(o)), 1)
        self.assertEqual(len(self.wakes(o)), 1)

    def test_the_run_is_recycled_after_max_messages(self):
        o = self.make()
        self.live_run(o)
        session.start(o, [], o.clock())
        for i in range(session.MAX_MESSAGES):
            self.ask(o, f"m{i}")
            console.tick(o, o.clock())
        self.assertEqual(len(self.to_console(o)), session.MAX_MESSAGES)
        self.assertFalse(any(s[2].get("end") for s in self.to_console(o)))
        console.tick(o, o.clock())
        ends = [s for s in self.to_console(o) if s[2].get("end")]
        self.assertEqual(len(ends), 1)
        console.tick(o, o.clock())               # asked once only
        self.assertEqual(len([s for s in self.to_console(o) if s[2].get("end")]), 1)

    def test_an_idle_run_is_asked_to_end_after_15_minutes(self):
        o = self.make()
        self.live_run(o)
        session.start(o, [], o.clock())
        console.tick(o, o.clock())
        self.assertEqual(self.to_console(o), [])
        self.clock.offset = session.IDLE_S + 1
        console.tick(o, o.clock())
        self.assertEqual([s[2].get("end") for s in self.to_console(o)], [True])

    def test_an_unanswered_message_keeps_the_run_from_idling_out(self):
        o = self.make()
        self.live_run(o)
        session.start(o, [], o.clock())
        self.ask(o, "x")
        self.clock.offset = session.IDLE_S + 1
        console.tick(o, o.clock())
        self.assertEqual([s[2].get("text") for s in self.to_console(o)], ["x"])

    def test_after_the_run_ends_the_next_message_launches_a_fresh_session(self):
        o = self.make()
        self.live_run(o)
        session.start(o, [], o.clock())
        session._request_end(o, "idle")
        o.store.set_run("rc", ended=2.0)
        self.ask(o, "volta")
        console.tick(o, o.clock())
        self.assertEqual(len(self.wakes(o)), 1)
        self.assertEqual(o.store.get_meta("console_sess_end", "0"), "0")
        self.assertEqual(o.store.get_meta("console_sess_n", "0"), "1")


class Partial(support.DaemonCase):
    def reply(self, o, **body):
        console.on_reply(o, {"id": "r" + str(len(S.history(o.hdir))) + str(body), "body": body})

    def test_a_partial_reply_is_shown_until_the_final_one(self):
        o = self.make()
        m = S.add_message(o.hdir, "user", "status?")
        self.reply(o, req=m["id"], text="vendo os PRs", partial=True)
        view = console_chat.view(o.hdir)
        self.assertEqual(view["partial"]["text"], "vendo os PRs")
        self.assertEqual([x["kind"] for x in view["messages"]], ["user"])
        self.assertEqual(S.unanswered(o.hdir)[0]["id"], m["id"])   # still open
        self.reply(o, req=m["id"], text="tudo certo")
        view = console_chat.view(o.hdir)
        self.assertIsNone(view["partial"])
        self.assertEqual(view["messages"][-1]["text"], "tudo certo")

    def test_a_partial_of_an_answered_message_is_not_shown(self):
        o = self.make()
        m = S.add_message(o.hdir, "user", "q")
        S.set_partial(o.hdir, m["id"], "meio")
        S.add_message(o.hdir, "orch", "pronto", req=m["id"])
        self.assertIsNone(console_chat.view(o.hdir)["partial"])


if __name__ == "__main__":
    unittest.main()

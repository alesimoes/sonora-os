"""The confirm step (console_actions.confirm/cancel/retry) and the run that
answers (console.tick): nothing executes before Confirmar, a card runs once,
every decision is in the audit log."""
import os
import unittest
from unittest import mock

import support
from harness import console, console_actions as A, console_door, console_store as S, ownerchat


def events(o):
    return [e["event"] for e in S.audit_tail(o.hdir)]


class Confirm(support.DaemonCase):
    def card(self, o, verb, **args):
        return A.propose(o.hdir, verb, args, o.clock())["id"]

    def state(self, o, aid):
        return S.load_actions(o.hdir)[aid]["state"]

    def test_nothing_runs_until_confirm_and_a_card_runs_once(self):
        o = self.make()
        aid = self.card(o, "stop")
        ownerchat.tick(o)
        self.assertFalse(os.path.exists(o._p("STOP")))
        console_door.post(o.hdir, "confirm", action_id=aid)
        ownerchat.tick(o)
        self.assertTrue(os.path.exists(o._p("STOP")) and os.path.exists(o._p("STOP.NOW")))
        self.assertEqual(self.state(o, aid), "done")
        os.remove(o._p("STOP"))
        console_door.post(o.hdir, "confirm", action_id=aid)     # a second tap
        ownerchat.tick(o)
        self.assertFalse(os.path.exists(o._p("STOP")))
        self.assertEqual(events(o), ["proposed", "confirmed", "done", "refused"])

    def test_cancel_stops_the_card_from_ever_running(self):
        o = self.make()
        aid = self.card(o, "drain")
        A.cancel(o, aid)
        A.confirm(o, aid)
        self.assertEqual(self.state(o, aid), "cancelled")
        self.assertFalse(os.path.exists(o._p("STOP")))

    def test_a_card_older_than_15_minutes_expires_instead_of_running(self):
        o = self.make()
        aid = self.card(o, "drain")
        self.clock.offset = S.PENDING_TTL_S + 1
        A.confirm(o, aid)
        self.assertEqual(self.state(o, aid), "expired")
        self.assertFalse(os.path.exists(o._p("STOP")))

    def test_the_sweep_expires_old_pending_cards(self):
        o = self.make()
        aid = self.card(o, "drain")
        self.clock.offset = S.PENDING_TTL_S + 1
        console.tick(o, o.clock())
        self.assertEqual(self.state(o, aid), "expired")

    def test_an_unknown_card_is_refused_and_said(self):
        o = self.make()
        A.confirm(o, "a999")
        self.assertEqual(events(o), ["refused"])
        self.assertEqual(S.history(o.hdir)[-1]["kind"], "system")

    def test_a_failing_action_is_failed_not_a_crash_and_retry_needs_a_new_confirm(self):
        o = self.make()
        aid = self.card(o, "task.close", task="nope")
        A.confirm(o, aid)
        self.assertEqual(self.state(o, aid), "failed")
        A.retry(o, aid)
        new = [a for a in S.load_actions(o.hdir).values() if a["id"] != aid][0]
        self.assertEqual(new["state"], "pending")
        self.assertNotEqual(new["id"], aid)
        A.retry(o, new["id"])        # only a failed card can be retried
        self.assertEqual(len(S.load_actions(o.hdir)), 2)

    def test_pause_and_resume_a_pool_use_the_cli_files(self):
        o = self.make()
        A.confirm(o, self.card(o, "pool.pause", pool="build"))
        self.assertEqual(o.paused(), {"build"})
        A.confirm(o, self.card(o, "pool.resume", pool="build"))
        self.assertEqual(o.paused(), set())

    def test_task_add_requires_a_known_agent_and_a_free_id(self):
        o = self.make()
        aid = self.card(o, "task.add", id="n1", track="daw", agent="rt-engineer", brief="b")
        A.confirm(o, aid)
        self.assertEqual(self.state(o, aid), "failed")           # no agent file in this root
        os.makedirs(os.path.join(o.root, ".claude", "agents"))
        open(os.path.join(o.root, ".claude", "agents", "rt-engineer.md"), "w").close()
        A.confirm(o, self.card(o, "task.add", id="n1", track="daw", agent="rt-engineer", brief="b"))
        self.assertEqual(o.store.get_task("n1")["state"], "queued")

    def test_run_stop_ends_the_run_and_holds_its_task(self):
        o = self.make()
        o.store.add_task("t1", "daw", "rt-engineer")
        o.store.add_run(id="r1", task_id="t1", role="engineer", name="eng-1", pid=4242, started=1.0)
        with mock.patch("harness.launcher.kill_tree") as kill:
            A.confirm(o, self.card(o, "run.stop", name="eng-1"))
        kill.assert_called_once_with(4242)
        self.assertEqual(o.store.active_runs(), [])
        self.assertGreater(o.store.get_task("t1")["not_before"], o.clock() + 1000)

    def test_requeue_a_blocked_task_and_close_a_task(self):
        o = self.make()
        o.store.add_task("t1", "daw", "rt-engineer")
        o.store.advance("t1", "blocked", note="x")
        A.confirm(o, self.card(o, "task.requeue", task="t1"))
        self.assertEqual(o.store.get_task("t1")["state"], "queued")
        A.confirm(o, self.card(o, "task.close", task="t1"))
        self.assertEqual(o.store.get_task("t1")["state"], "closed")

    def test_agent_ask_and_wake_go_through_ownerchat(self):
        o = self.make()
        o.store.add_task("t1", "daw", "rt-engineer")
        o.store.add_run(id="r1", task_id="t1", role="engineer", name="eng-1", started=1.0, ended=2.0)
        A.confirm(o, self.card(o, "agent.ask", name="eng-1", text="status?"))
        self.assertEqual([(s[0], s[1]) for s in o.broker.sent], [("eng-1", "owner_ask")])
        aid = self.card(o, "agent.wake", name="eng-1")
        A.confirm(o, aid)
        self.assertEqual(self.state(o, aid), "done")

    def test_pr_adopt_calls_the_train(self):
        o = self.make()
        o.train.adopt = mock.Mock(return_value=[7])
        aid = self.card(o, "pr.adopt", prs=[7])
        A.confirm(o, aid)
        o.train.adopt.assert_called_once_with({7})
        self.assertEqual(S.load_actions(o.hdir)[aid]["result"], "adopted #7")

    def test_every_decision_is_an_event_of_the_daemon(self):
        o = self.make()
        A.confirm(o, self.card(o, "drain"))
        self.assertIn("console_action", [e["kind"] for e in o.store.events(kind="console_action")])


class AnsweringRun(support.DaemonCase):
    def ask(self, o, text="oi"):
        console_door.post(o.hdir, "console", text=text)
        ownerchat.tick(o)

    def waking(self, o):
        return [w for w in o.store.tasks(kind="wake") if w["agent"] == "console"]

    def test_one_console_run_per_unanswered_message_and_never_two_at_once(self):
        o = self.make()
        console.tick(o, o.clock())
        self.assertEqual(self.waking(o), [])
        self.ask(o)
        console.tick(o, o.clock())
        console.tick(o, o.clock())
        self.assertEqual(len(self.waking(o)), 1)
        self.assertEqual(self.waking(o)[0]["event"], "console_request")

    def test_an_answered_message_starts_no_run(self):
        o = self.make()
        self.ask(o)
        console.on_reply(o, {"id": "x", "body": {"text": "ok"}})
        console.tick(o, o.clock())
        self.assertEqual(self.waking(o), [])

    def test_gives_up_after_two_runs_without_a_reply(self):
        o = self.make()
        self.ask(o)
        for _ in range(console.MAX_LAUNCHES):
            console.tick(o, o.clock())
            for w in self.waking(o):
                o.store.set_task(w["id"], state="merged")
        console.tick(o, o.clock())
        self.assertEqual(len(self.waking(o)), console.MAX_LAUNCHES)
        self.assertEqual(S.history(o.hdir)[-1]["kind"], "system")
        self.assertEqual(S.unanswered(o.hdir), [])

    def test_no_run_while_a_console_run_is_active(self):
        o = self.make()
        o.store.add_task("w", None, "console", kind="wake")
        o.store.set_task("w", state="merged")
        o.store.add_run(id="rc", task_id="w", role="planner", name="console", started=1.0)
        self.ask(o)
        console.tick(o, o.clock())
        self.assertEqual([w for w in self.waking(o) if w["state"] == "queued"], [])


if __name__ == "__main__":
    unittest.main()

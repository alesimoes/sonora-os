"""Owner chat from the dashboard (harness/ownerchat.py): message type, the
daemon consuming ask/wake requests, and the thread view."""
import json
import os
import shutil
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import ownerchat
from harness import protocol as P


class FakeStore:
    def __init__(self, runs, tasks):
        self.runs, self.tasks, self.events, self.updates = runs, tasks, [], []

    def last_run_of_name(self, name):
        rs = [r for r in self.runs if r["name"] == name]
        return rs[-1] if rs else None

    def active_runs(self):
        return [r for r in self.runs if r.get("ended") is None]

    def get_task(self, tid):
        return self.tasks.get(tid)

    def set_task(self, tid, **f):
        self.updates.append((tid, f))

    def event(self, tid, kind, detail=""):
        self.events.append((tid, kind, detail))


class FakeOrch:
    def __init__(self, hdir, runs, tasks):
        self.hdir, self.sent = hdir, []
        self.store = FakeStore(runs, tasks)

    def clock(self):
        return 500.0

    def _tell(self, to, type_, body, feature=None, task=None):
        self.sent.append((to, type_, body, feature, task))


class Daemon(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def orch(self, ended=None, state="building"):
        runs = [{"name": "eng-1", "task_id": "t1", "ended": ended}]
        return FakeOrch(self.tmp, runs, {"t1": {"state": state, "feature_id": "f1"}})

    def test_owner_ask_is_a_message_type(self):
        self.assertIn("owner_ask", P.MSG_TYPES)
        self.assertEqual(P.PROTOCOL_VERSION, 1)

    def test_ask_is_sent_to_the_agent_with_the_reply_hint(self):
        o = self.orch()
        ownerchat.write_request(self.tmp, "ask", "eng-1", "what are you doing?")
        self.assertEqual(ownerchat.pending(self.tmp, "eng-1"), 1)
        ownerchat.tick(o)
        to, type_, body, feat, task = o.sent[0]
        self.assertEqual((to, type_, feat, task), ("eng-1", "owner_ask", "f1", "t1"))
        self.assertEqual(body["text"], "what are you doing?")
        self.assertIn("reply_to", body["reply"])
        self.assertEqual(ownerchat.pending(self.tmp, "eng-1"), 0)

    def test_unknown_agent_is_rejected_not_sent(self):
        o = self.orch()
        ownerchat.write_request(self.tmp, "ask", "ghost", "hi")
        ownerchat.tick(o)
        self.assertEqual(o.sent, [])
        self.assertEqual(o.store.events[0][1], "owner_request_rejected")

    def test_wake_relaunches_a_dead_run_now(self):
        o = self.orch(ended=400.0)
        ownerchat.write_request(self.tmp, "wake", "eng-1")
        ownerchat.tick(o)
        self.assertEqual(o.store.updates, [("t1", {"not_before": 500.0})])

    def test_wake_leaves_a_running_agent_alone(self):
        o = self.orch()
        ownerchat.write_request(self.tmp, "wake", "eng-1")
        ownerchat.tick(o)
        self.assertEqual(o.store.updates, [])
        self.assertIn("already running", o.store.events[0][2])

    def test_wake_does_not_unblock_a_blocked_task(self):
        o = self.orch(ended=400.0, state="blocked")
        ownerchat.write_request(self.tmp, "wake", "eng-1")
        ownerchat.tick(o)
        self.assertEqual(o.store.updates, [])

    def test_bad_requests_raise(self):
        for args in (("shout", "eng-1", "x"), ("ask", "../x", "x"), ("ask", "eng-1", "  ")):
            with self.assertRaises(ValueError):
                ownerchat.write_request(self.tmp, *args)

    def test_thread_pairs_ask_and_answer_and_ignores_others(self):
        os.makedirs(os.path.join(self.tmp, "log"))
        rows = [
            {"id": "1", "ts": "t1", "from": "daemon", "to": "eng-1", "type": "owner_ask", "body": {"text": "q"}},
            {"id": "2", "ts": "t2", "from": "eng-1", "to": "daemon", "type": "answer", "reply_to": "1", "body": {"text": "a"}},
            {"id": "3", "ts": "t3", "from": "eng-2", "to": "daemon", "type": "note", "body": {"text": "other"}},
            {"id": "4", "ts": "t4", "from": "eng-1", "to": "tester", "type": "note", "body": {"text": "private"}},
        ]
        with open(os.path.join(self.tmp, "log", "messages.jsonl"), "w") as f:
            f.write("\n".join(json.dumps(r) for r in rows) + "\nnot json\n")
        got = ownerchat.thread(self.tmp, "eng-1")
        self.assertEqual([(m["who"], m["text"], m["reply_to"]) for m in got],
                         [("owner", "q", None), ("eng-1", "a", "1")])


class InTheTick(support.DaemonCase):
    def test_the_daemon_tick_consumes_requests(self):
        o = self.make(floors={"daw": 0})
        o.store.add_task("t1", "daw", "rt-engineer")
        o.store.add_run(id="r1", task_id="t1", role="engineer", name="eng-1",
                        started=1.0, ended=2.0)
        ownerchat.write_request(o.hdir, "ask", "eng-1", "status?")
        o.tick()
        self.assertIn(("eng-1", "owner_ask"), [(s[0], s[1]) for s in o.broker.sent])
        self.assertEqual(os.listdir(ownerchat.request_dir(o.hdir)), [])


if __name__ == "__main__":
    unittest.main()

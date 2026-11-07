"""Owner chat from the dashboard (harness/ownerchat.py): message type, the
daemon consuming ask/wake requests, the thread view, and the dashboard's
POST endpoint against a local server with a fake broker log (no daemon)."""
import json
import os
import shutil
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

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


class Endpoint(unittest.TestCase):
    """The real Handler on a free port, reading a fake orchestrator tree."""
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(odir)
        self.status = os.path.join(odir, "status.json")
        with open(self.status, "w") as f:
            f.write("{}")
        with open(os.path.join(odir, "state.json"), "w") as f:
            json.dump({"runs": [{"id": "r1", "name": "eng-1", "task_id": "t1", "role": "engineer"}]}, f)
        os.makedirs(os.path.join(self.tmp, ".harness", "log"))
        self.mock = os.path.join(self.tmp, "mockups")
        os.makedirs(self.mock)
        with open(os.path.join(self.mock, "index.html"), "w") as f:
            f.write("<h1>gallery</h1>")
        with open(os.path.join(self.tmp, "secret.txt"), "w") as f:
            f.write("no")
        import dashboard
        import dashboard_links
        self.env = dict(os.environ)
        os.environ["SONORA_ORCH_STATUS"] = self.status
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(self.env)))
        self.orig = dashboard_links.mockups_dir
        dashboard_links.mockups_dir = lambda: self.mock
        self.addCleanup(setattr, dashboard_links, "mockups_dir", self.orig)
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]

    def call(self, path, payload=None, ctype="application/json", method=None):
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(self.url + path, data=data, method=method,
                                     headers={"Content-Type": ctype} if data else {})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, r.read(), r.headers.get("Content-Type")
        except urllib.error.HTTPError as e:
            return e.code, e.read(), None

    def test_ask_queues_a_request_for_the_daemon(self):
        code, body, _ = self.call("/api/agent/ask", {"name": "eng-1", "text": "status?"})
        self.assertEqual(code, 202)
        reqs = os.listdir(os.path.join(self.tmp, ".harness", "owner-requests"))
        self.assertEqual(len(reqs), 1)
        with open(os.path.join(self.tmp, ".harness", "owner-requests", reqs[0])) as f:
            self.assertEqual(json.load(f), {"kind": "ask", "to": "eng-1", "text": "status?"})

    def test_unknown_agent_and_empty_text_are_refused(self):
        self.assertEqual(self.call("/api/agent/ask", {"name": "ghost", "text": "x"})[0], 404)
        self.assertEqual(self.call("/api/agent/ask", {"name": "eng-1", "text": ""})[0], 400)
        self.assertEqual(self.call("/api/agent/nope", {"name": "eng-1"})[0], 404)

    def test_non_json_post_is_refused(self):
        code, _, _ = self.call("/api/agent/ask", {"name": "eng-1", "text": "x"}, ctype="text/plain")
        self.assertEqual(code, 415)

    def test_stale_status_means_daemon_is_down(self):
        os.utime(self.status, (1, 1))
        self.assertEqual(self.call("/api/agent/wake", {"name": "eng-1"})[0], 503)

    def test_run_detail_has_the_thread_and_the_agent_panel_note(self):
        with open(os.path.join(self.tmp, ".harness", "log", "messages.jsonl"), "w") as f:
            f.write(json.dumps({"id": "1", "ts": "t", "from": "eng-1", "to": "daemon",
                                "type": "answer", "body": {"text": "building"}}) + "\n")
        _, body, _ = self.call("/api/detail?kind=run&id=r1")
        chat = json.loads(body)["chat"]
        self.assertEqual((chat["name"], chat["daemon_alive"]), ("eng-1", True))
        self.assertEqual(chat["thread"][0]["text"], "building")
        _, body, _ = self.call("/api/detail?kind=agent&id=nobody")
        self.assertIn("Agent", json.loads(body)["not_reachable"])

    def test_links_and_local_gallery(self):
        _, body, _ = self.call("/api/state")
        titles = [(l["title"], l["url"]) for l in json.loads(body)["links"]]
        self.assertIn(("Faceplates (round 4)", "https://claude.ai/artifact/TwaXYZD42ZRm2At3Fe7SoK"), titles)
        self.assertIn(("Faceplates (local)", "/mockups/"), titles)
        code, body, ctype = self.call("/mockups/")
        self.assertEqual((code, body, ctype), (200, b"<h1>gallery</h1>", "text/html; charset=utf-8"))
        self.assertEqual(self.call("/mockups/../secret.txt")[0], 404)
        self.assertEqual(self.call("/mockups/missing.png")[0], 404)


if __name__ == "__main__":
    unittest.main()

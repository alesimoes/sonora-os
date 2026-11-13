"""The console screen's data (dashboard_console.py) and its endpoints
(dashboard_console_http.py + dashboard.Handler) against a fake orchestrator
tree: the status strip rules, the attention queue, token and host checks,
and that a POST only drops a request at the daemon's door."""
import json
import os
import shutil
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

import support  # noqa: F401  (sets sys.path)
import dashboard_console as V
from harness import console_store as S

NOW = 100000.0


def orch(**kw):
    d = {"updated_ts": NOW - 5, "cap": 8, "runs_last_hour": 3, "max_runs_per_hour": 6,
         "open_prs": 2, "max_open_prs": 8}
    d.update(kw)
    return d


def agent(name, since=NOW - 3600, last=NOW - 10, **kw):
    return dict({"name": name, "what": "task of " + name, "model": "opus", "role_pt": "Engenheiro",
                 "since": since, "last": last, "pr": None}, **kw)


class Strip(unittest.TestCase):
    def test_levels_at_the_edges(self):
        self.assertEqual(V.level_of(5, 8), "ok")
        self.assertEqual(V.level_of(7, 8), "warn")      # 87 %
        self.assertEqual(V.level_of(8, 8), "bad")
        self.assertEqual(V.level_of(1, 8, brake=True), "bad")
        self.assertEqual(V.level_of(3, 0), "ok")

    def test_daemon_cell_by_age(self):
        # One tick is ~50 s (30 s sleep + work): a normal tick must read ok.
        for age, text in ((5, "vivo 5s"), (90, "vivo 90s"), (300, "lento 300s"), (900, "travado 15min")):
            self.assertEqual(V.daemon_cell({"updated_ts": NOW - age}, NOW)["text"], text)
        self.assertEqual(V.daemon_cell({"updated_ts": NOW - 300}, NOW, alive=False),
                         {"level": "bad", "text": "parado"})
        self.assertEqual(V.daemon_cell(None, NOW), {"level": "bad", "text": "parado"})

    def test_cells_read_the_status_and_the_open_pr_brake(self):
        v = V.build({"orchestrator": orch(open_prs=8), "board": {"agents": [agent("eng-1")], "prs": []}}, True, NOW)
        self.assertEqual((v["strip"]["agents"]["text"], v["strip"]["runs"]["text"]), ("1/8", "3/6"))
        self.assertEqual(v["strip"]["prs"]["level"], "bad")
        self.assertEqual(v["strip"]["alerts"], {"n": 0, "level": "ok", "text": "0"})


class Agents(unittest.TestCase):
    def test_longest_running_first_with_real_model_and_no_signal(self):
        rows = V.agent_rows([agent("eng-2", since=NOW - 600), agent("eng-1", since=NOW - 4380, last=NOW - 200,
                                                                    pr=528), agent("console")], NOW)
        self.assertEqual([r["id"] for r in rows], ["eng-1", "eng-2"])
        self.assertEqual((rows[0]["minutes"], rows[0]["model"], rows[0]["pr"]), (73, "opus", 528))
        self.assertEqual(rows[0]["no_signal_min"], 3)
        self.assertIsNone(rows[1]["no_signal_min"])

    def test_the_console_run_is_not_listed_as_a_worker(self):
        self.assertEqual(V.agent_rows([agent("console")], NOW), [])


class Attention(unittest.TestCase):
    def queue(self, o=None, agents=(), prs=()):
        v = V.build({"orchestrator": o or orch(), "board": {"agents": list(agents), "prs": list(prs)}}, True, NOW)
        return v["attention"]

    def test_empty_when_nothing_needs_the_owner(self):
        self.assertEqual(self.queue(), [])

    def test_silent_agent_gets_a_restart_button_and_goes_away_with_the_cause(self):
        q = self.queue(agents=[agent("eng-104", last=NOW - 2400)])
        self.assertEqual((q[0]["level"], q[0]["button"]["label"]), ("bad", "Reiniciar eng-104"))
        self.assertEqual(self.queue(agents=[agent("eng-104", last=NOW - 20)]), [])
        self.assertEqual(self.queue(agents=[agent("eng-104", last=NOW - 540)]), [])   # supervisor still on it

    def test_green_pr_without_merge_owner_decision_alert_and_stop_switch(self):
        o = orch(waits_on_owner=[{"task": "t9", "title": "ADR"}], kill_switch="STOP",
                 alerts=[{"id": "disk", "check": "disk", "level": "alert", "text": "5 GB livres", "since": NOW - 600}])
        q = self.queue(o, prs=[{"number": 544, "late": True, "late_text": "verde há 18 min", "green_min": 18}])
        self.assertEqual(q[0]["key"], "alert:disk")                 # bad before warn
        self.assertEqual({i["key"] for i in q}, {"alert:disk", "switch", "pr:544", "owner:t9"})
        self.assertTrue(all(i["button"] or i["key"].startswith("alert") for i in q))
        self.assertEqual([i["level"] for i in q], sorted((i["level"] for i in q), key=lambda x: x != "bad"))

    def test_daemon_down_is_an_item_without_a_button(self):
        q = self.queue(orch(updated_ts=NOW - 900))
        self.assertEqual((q[0]["key"], q[0]["button"]), ("daemon", None))


class Served(unittest.TestCase):
    """The real Handler on a free port; the daemon is 'alive' by a fresh status."""
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(odir)
        self.status = os.path.join(odir, "status.json")
        with open(self.status, "w") as f:
            f.write("{}")
        self.hdir = os.path.join(self.tmp, ".harness")
        os.makedirs(self.hdir)
        self.env = dict(os.environ)
        os.environ["SONORA_ORCH_STATUS"] = self.status
        self.addCleanup(lambda: (os.environ.clear(), os.environ.update(self.env)))
        import dashboard
        from dashboard_token import TOKEN
        self.token = TOKEN
        self.srv = dashboard.Server(("127.0.0.1", 0), dashboard.Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.addCleanup(self.srv.server_close)
        self.addCleanup(self.srv.shutdown)
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]

    def call(self, path, payload=None, ctype="application/json", token=True):
        data = json.dumps(payload).encode() if payload is not None else None
        headers = {"Content-Type": ctype} if data else {}
        if data and token:
            headers["X-Dash-Token"] = self.token
        req = urllib.request.Request(self.url + path, data=data, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read() or b"{}") if "json" in r.headers.get("Content-Type", "") else r.read()
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def test_a_burst_of_connections_is_served_not_refused(self):
        """The phone through tailscale fetches every file at once (2026-11-09:
        a 5-deep accept queue refused some and the chat never loaded)."""
        import concurrent.futures as cf

        def get(_):
            try:
                with urllib.request.urlopen(self.url + "/static/console.css", timeout=10) as r:
                    return r.status
            except OSError as e:
                return type(e).__name__
        with cf.ThreadPoolExecutor(128) as ex:
            got = list(ex.map(get, range(128)))
        self.assertEqual(got, [200] * 128)
        # The burst does not always overflow a short queue on a fast test
        # server; the queue depth itself is the guard.
        import dashboard
        self.assertGreaterEqual(dashboard.Server.request_queue_size, 64)

    def test_a_second_dashboard_cannot_share_the_port(self):
        import dashboard
        with self.assertRaises(OSError):
            dashboard.Server(("127.0.0.1", self.srv.server_address[1]), dashboard.Handler)

    def requests(self):
        d = os.path.join(self.hdir, "owner-requests")
        out = []
        for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            with open(os.path.join(d, n), encoding="utf-8") as f:
                out.append(json.load(f))
        return out

    def test_the_page_is_served_with_the_token_and_the_view_has_the_chat(self):
        code, body = self.call("/")
        self.assertEqual(code, 200)
        self.assertIn(self.token.encode(), body)
        S.add_message(self.hdir, "user", "oi")
        code, view = self.call("/api/console")
        self.assertEqual(code, 200)
        self.assertEqual([m["text"] for m in view["chat"]["messages"]], ["oi"])
        self.assertIn("strip", view)

    def test_a_message_only_drops_a_request_for_the_daemon(self):
        code, body = self.call("/api/chat", {"text": "status", "to": None})
        self.assertEqual(code, 202)
        self.assertEqual([{k: v for k, v in r.items() if k != "sent"} for r in self.requests()],
                         [{"kind": "console", "text": "status", "to": None}])
        self.assertEqual(S.history(self.hdir), [])       # the dashboard never writes the chat

    def test_confirm_cancel_retry_post_the_decision_and_run_nothing(self):
        for kind in ("confirm", "cancel", "retry"):
            self.assertEqual(self.call("/api/action/a123/" + kind, {})[0], 202)
        self.assertEqual([r["kind"] for r in self.requests()], ["confirm", "cancel", "retry"])
        self.assertEqual(self.call("/api/action/a123/undo", {})[0], 404)
        self.assertEqual(self.call("/api/action/zzz/confirm", {})[0], 400)

    def test_posts_need_the_token_json_and_a_local_host(self):
        self.assertEqual(self.call("/api/chat", {"text": "x"}, token=False)[0], 403)
        self.assertEqual(self.call("/api/chat", {"text": "x"}, ctype="text/plain")[0], 415)
        req = urllib.request.Request(self.url + "/api/chat", data=b"{}", headers={
            "Content-Type": "application/json", "X-Dash-Token": self.token, "Host": "evil.example"})
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(cm.exception.code, 403)
        self.assertEqual(self.requests(), [])

    def test_with_the_daemon_down_nothing_is_queued(self):
        os.utime(self.status, (1, 1))
        code, body = self.call("/api/chat", {"text": "status"})
        self.assertEqual(code, 503)
        self.assertIn("daemon", body["error"])
        self.assertEqual(self.requests(), [])

    def test_empty_text_and_old_routes_are_refused(self):
        self.assertEqual(self.call("/api/chat", {"text": "  "})[0], 400)
        self.assertEqual(self.call("/api/action", {"action": "run.stop"})[0], 404)
        self.assertEqual(self.call("/api/agent/ask", {"name": "x", "text": "y"})[0], 404)


if __name__ == "__main__":
    unittest.main()

"""Dashboard actions: the registry, parameter validation, confirmation, the
token on every POST and the audit log. Every subprocess is faked
(dashboard_proc), and the orchestrator's files live in a temp directory."""
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.request
from http.server import ThreadingHTTPServer
from unittest import mock

import support  # noqa: F401  (sets sys.path)

import dashboard
import dashboard_actions as A
import dashboard_actions_agents
import dashboard_actions_ci as CI
import dashboard_actions_http as H
import dashboard_actions_prs as PR
import dashboard_actions_sys as SYS
import dashboard_audit
import dashboard_proc
import dashboard_state


REAL_RUN = dashboard_proc.run_cmd


class Fake:
    """Replaces dashboard_proc.run_cmd and records the commands."""
    def __init__(self, rc=0, out="fake output"):
        self.rc, self.out, self.calls = rc, out, []

    def __call__(self, cmd, timeout, cwd=None):
        self.calls.append(list(cmd))
        return self.rc, self.out


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(odir)
        self.odir = odir
        env = mock.patch.dict(os.environ, {
            "SONORA_ORCH_STATUS": os.path.join(odir, "status.json"),
            "SONORA_DASH_AUDIT": os.path.join(self.tmp, "audit.log"),
            "SONORA_HEAVY_DIR": os.path.join(self.tmp, "lock")})
        env.start()
        self.addCleanup(env.stop)
        self.fake = Fake()
        p = mock.patch.object(dashboard_proc, "run_cmd", self.fake)
        p.start()
        self.addCleanup(p.stop)
        A.JOBS.clear()

    def write(self, name, obj):
        with open(os.path.join(self.odir, name), "w", encoding="utf-8") as f:
            json.dump(obj, f)

    def go(self, action, params=None, confirm=False):
        code, body = A.submit(action, params or {}, confirm, wait=True)
        return code, body, (A.job_view(body["job"]) if code == 202 else None)


class Registry(Base):
    def test_allowlist_unknown_action_and_params_shape(self):
        self.assertEqual(A.submit("rm.rf", {})[0], 404)
        self.assertEqual(A.submit("daemon.status", "x")[0], 400)
        names = {a["name"] for a in A.catalogue()}
        for n in ("run.stop", "run.relaunch", "pr.adopt", "pr.merge", "ci.cancel", "lock.remove", "pool.pause",
                  "pool.resume", "daemon.status", "daemon.restart", "daemon.update", "supervisor.start",
                  "supervisor.stop", "disk.preview", "disk.clean"):
            self.assertIn(n, names)

    def test_destructive_needs_confirmation_flag(self):
        self.write("status.json", {"pools": {"build": {}}})
        code, body = A.submit("daemon.restart", {})
        self.assertEqual((code, body.get("needs_confirm")), (409, True))
        self.assertEqual(self.fake.calls, [])
        self.assertEqual(A.submit("daemon.restart", {}, confirm="yes")[0], 409)   # only a real true counts

    def test_non_destructive_runs_without_confirmation_and_is_audited(self):
        self.write("status.json", {"pools": {"build": {}}})
        code, _, job = self.go("pool.pause", {"pool": "build", "evil": "; rm -rf /"})
        self.assertEqual(code, 202)
        self.assertEqual(job["params"], {"pool": "build"})        # undeclared parameters are dropped
        self.assertEqual(job["state"], "done")
        self.assertIn("pause", self.fake.calls[0])
        self.assertEqual(self.fake.calls[0][-2:], ["--pool", "build"])
        row = dashboard_audit.recent()[0]
        self.assertEqual((row["action"], row["params"], row["result"], row["ok"]), ("pool.pause", {"pool": "build"}, "done", True))
        self.assertEqual(row["output"], "fake output")

    def test_failed_command_is_a_failed_job_and_logged(self):
        self.write("status.json", {"pools": {"build": {}}})
        self.fake.rc, self.fake.out = 3, "boom"
        _, _, job = self.go("pool.resume", {"pool": "build"})
        self.assertEqual((job["state"], job["ok"]), ("failed", False))
        self.assertEqual(self.fake.calls[0][-1], "--resume")
        self.assertFalse(dashboard_audit.recent()[0]["ok"])

    def test_runner_exception_does_not_escape(self):
        A.register("t.boom", "t", "t", lambda p: {}, lambda p, t: 1 / 0)
        self.addCleanup(A.REGISTRY.pop, "t.boom")
        _, _, job = self.go("t.boom")
        self.assertEqual(job["state"], "failed")
        self.assertIn("ZeroDivisionError", job["output"])

    def test_same_action_twice_at_once_is_refused(self):
        gate = threading.Event()
        A.register("t.slow", "t", "t", lambda p: {}, lambda p, t: (gate.wait(5), "ok"))
        self.addCleanup(A.REGISTRY.pop, "t.slow")
        self.assertEqual(A.submit("t.slow", {})[0], 202)
        self.assertEqual(A.submit("t.slow", {})[0], 409)
        gate.set()

    def test_runner_receives_the_action_timeout(self):
        seen = []
        A.register("t.to", "t", "t", lambda p: {}, lambda p, t: (seen.append(t), "ok"), timeout=7)
        self.addCleanup(A.REGISTRY.pop, "t.to")
        self.go("t.to")
        self.assertEqual(seen, [7])

    def test_real_command_timeout_is_a_failure_not_a_hang(self):
        self.assertEqual(REAL_RUN(["definitely-not-a-program-xyz"], 2)[0], 127)
        rc, out = REAL_RUN([sys.executable, "-c", "import time; time.sleep(30)"], 1)
        self.assertEqual(rc, 124)


class Audit(Base):
    def test_recent_is_newest_first_and_survives_garbage(self):
        dashboard_audit.record("a", {}, "done", "x" * 5000, True)
        with open(dashboard_audit.log_path(), "a") as f:
            f.write("not json\n")
        dashboard_audit.record("b", {"k": 1}, "failed", "y", False)
        rows = dashboard_audit.recent()
        self.assertEqual([r["action"] for r in rows], ["b", "a"])
        self.assertLessEqual(len(rows[1]["output"]), dashboard_audit.KEEP_OUTPUT)

    def test_default_path_is_under_dot_harness(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("SONORA_DASH_AUDIT")
            p = dashboard_audit.log_path().replace("\\", "/")
        self.assertTrue(p.endswith("/.harness/dashboard-actions.log"), p)


class Prs(Base):
    def setUp(self):
        super().setUp()
        dashboard_state.state["prs"] = [{"number": 7}, {"number": 8}]
        self.addCleanup(dashboard_state.state.update, prs=[])

    def test_adopt_validates_against_open_prs_and_calls_the_cli(self):
        self.assertEqual(A.submit("pr.adopt", {"pr": 999})[0], 400)
        self.assertEqual(A.submit("pr.adopt", {"pr": "7; x"})[0], 400)
        self.assertEqual(A.submit("pr.adopt", {"pr": -1})[0], 400)
        code, _, job = self.go("pr.adopt", {"pr": "7"})
        self.assertEqual(code, 202)
        cmd = self.fake.calls[0]
        self.assertTrue(cmd[1].endswith("orchestrator.py"))
        self.assertEqual(cmd[-2:], ["adopt", "7"])

    def gh(self, **over):
        v = {"state": "OPEN", "mergeable": "MERGEABLE", "baseRefName": "main", "headRefOid": "abc123"}
        v.update(over)
        return mock.patch.object(PR, "gh_pr_view", lambda n, t: v)

    def test_merge_only_when_gate_green_and_github_agrees(self):
        self.write("status.json", {"train": [{"pr": 7, "phase": "verde", "head": "abc123"},
                                             {"pr": 8, "phase": "CI rodando", "head": "d"}]})
        with self.gh():
            self.assertEqual(A.submit("pr.merge", {"pr": 8}, True)[0], 400)           # gate not green
        for over in ({"mergeable": "CONFLICTING"}, {"state": "CLOSED"}, {"baseRefName": "other"}, {"headRefOid": "zzz"}):
            with self.gh(**over):
                self.assertEqual(A.submit("pr.merge", {"pr": 7}, True)[0], 400, over)
        self.assertEqual(self.fake.calls, [])

    def test_merge_pins_the_head_and_needs_confirmation(self):
        self.write("status.json", {"train": [{"pr": 7, "phase": "verde", "head": "abc123"}]})
        with self.gh():
            self.assertEqual(A.submit("pr.merge", {"pr": 7})[0], 409)
            self.assertEqual(self.fake.calls, [])
            _, _, job = self.go("pr.merge", {"pr": 7}, True)
        self.assertEqual(job["state"], "done")
        self.assertEqual(self.fake.calls[0], ["gh", "pr", "merge", "7", "--squash", "--match-head-commit", "abc123"])

    def test_merge_rechecks_when_the_head_moves_after_the_click(self):
        self.write("status.json", {"train": [{"pr": 7, "phase": "verde", "head": "abc123"}]})
        views = iter([{"state": "OPEN", "mergeable": "MERGEABLE", "baseRefName": "main", "headRefOid": "abc123"},
                      {"state": "OPEN", "mergeable": "MERGEABLE", "baseRefName": "main", "headRefOid": "new999"}])
        with mock.patch.object(PR, "gh_pr_view", lambda n, t: next(views)):
            _, _, job = self.go("pr.merge", {"pr": 7}, True)
        self.assertEqual(job["state"], "failed")
        self.assertEqual(self.fake.calls, [])


class Runs(Base):
    def state(self, runs):
        self.write("state.json", {"runs": runs})

    def test_stop_only_a_live_run_the_orchestrator_lists(self):
        self.state([{"name": "a-1", "pid": 4242}, {"name": "gone", "pid": 5, "ended": 1}])
        self.assertEqual(A.submit("run.stop", {"name": "nobody"}, True)[0], 400)
        self.assertEqual(A.submit("run.stop", {"name": "gone"}, True)[0], 400)
        with mock.patch.object(dashboard_actions_agents.launcher, "pid_alive", lambda p: False):
            self.assertEqual(A.submit("run.stop", {"name": "a-1"}, True)[0], 400)
        killed = []
        with mock.patch.object(dashboard_actions_agents.launcher, "pid_alive", lambda p: True), \
                mock.patch.object(dashboard_actions_agents.localci_cancel, "kill_tree", killed.append):
            self.assertEqual(A.submit("run.stop", {"name": "a-1"})[0], 409)   # destructive
            self.assertEqual(killed, [])
            _, _, job = self.go("run.stop", {"name": "a-1"}, True)
        self.assertEqual((job["state"], killed), ("done", [4242]))

    def test_relaunch_queues_a_wake_request(self):
        self.state([{"name": "a-1", "pid": 4242, "ended": 9}])
        self.assertEqual(A.submit("run.relaunch", {"name": "a-1"})[0], 400)       # no daemon
        self.write("status.json", {"pools": {}})
        wrote = []
        with mock.patch.object(dashboard_actions_agents.ownerchat, "write_request", lambda *a: wrote.append(a)), \
                mock.patch.object(dashboard_actions_agents.launcher, "pid_alive", lambda p: False):
            _, _, job = self.go("run.relaunch", {"name": "a-1"})
        self.assertEqual(job["state"], "done")
        self.assertEqual(wrote[0][1:3], ("wake", "a-1"))


class Ci(Base):
    def test_cancel_only_a_running_batch_with_a_live_pid(self):
        d = os.path.join(self.tmp, "ci")
        os.makedirs(d)
        meta = {"localci:k1": json.dumps({"state": "running", "pid": 77, "dir": d}),
                "localci:k2": json.dumps({"state": "passed", "pid": 78, "dir": d})}
        self.write("state.json", {"meta": meta})
        self.assertEqual(A.submit("ci.cancel", {"key": "k2"}, True)[0], 400)
        killed = []
        with mock.patch.object(CI.launcher, "pid_alive", lambda p: True), \
                mock.patch.object(CI.localci_cancel, "kill_tree", killed.append):
            _, _, job = self.go("ci.cancel", {"key": "k1"}, True)
        self.assertEqual((job["state"], killed), ("done", [77]))
        with open(os.path.join(d, "summary")) as f:
            self.assertIn("cancelled", f.read())   # never read as a pass

    def lock(self, rows):
        return mock.patch.object(CI, "heavy_lock_list", lambda: rows)

    LINE = "%s pid=%d kind=win user=u prio=2 start=1 cwd=/c/x cmd=%s"

    def test_lock_entry_removable_only_when_dead_or_not_a_job(self):
        rows = [self.LINE % ("holds", 11, "bash tools/local-ci.sh"), self.LINE % ("waits", 12, "sleep 1000")]
        with self.lock(rows):
            with mock.patch.object(CI, "pid_dead", lambda e: False):
                self.assertEqual(A.submit("lock.remove", {"pid": 11}, True)[0], 400)     # alive real job
                self.assertEqual(A.submit("lock.remove", {"pid": 12}, True)[0], 202)     # not a real job
            with mock.patch.object(CI, "pid_dead", lambda e: True):
                self.assertEqual(A.submit("lock.remove", {"pid": 11}, True)[0], 202)     # dead
            self.assertEqual(A.submit("lock.remove", {"pid": 99}, True)[0], 400)         # not in the lock
            self.assertEqual(A.submit("lock.remove", {"pid": 12})[0], 409)               # confirmation

    def test_lock_entry_files_are_removed(self):
        base = os.environ["SONORA_HEAVY_DIR"]
        os.makedirs(os.path.join(base, "slot-0"))
        os.makedirs(os.path.join(base, "wait"))
        with open(os.path.join(base, "slot-0", "owner"), "w") as f:
            f.write("pid=11\nkind=win\n")
        with open(os.path.join(base, "wait", "2-1-12"), "w") as f:
            f.write("pid=12\nkind=win\n")
        with self.lock([self.LINE % ("holds", 11, "x")]), mock.patch.object(CI, "pid_dead", lambda e: True):
            _, _, job = self.go("lock.remove", {"pid": 11}, True)
        self.assertEqual(job["state"], "done", job)
        self.assertFalse(os.path.exists(os.path.join(base, "slot-0")))
        self.assertTrue(os.path.exists(os.path.join(base, "wait", "2-1-12")))            # another pid is untouched


class Sys(Base):
    def test_pool_must_exist_in_status(self):
        self.write("status.json", {"pools": {"build": {}}})
        self.assertEqual(A.submit("pool.pause", {"pool": "nope"})[0], 400)
        self.assertEqual(A.submit("pool.pause", {"pool": "build --x"})[0], 400)

    def test_daemon_restart_stops_then_starts_detached(self):
        stopped, started = [], []
        with mock.patch.object(SYS, "daemon_pids", lambda: [9420]), \
                mock.patch.object(SYS, "stop_pids", lambda p: stopped.append(p) or True), \
                mock.patch.object(SYS, "start_daemon", lambda: started.append(1) or 555):
            _, _, job = self.go("daemon.restart", {}, True)
        self.assertEqual((job["state"], stopped, started), ("done", [[9420]], [1]))
        self.assertIn("555", job["output"])

    def test_daemon_restart_refuses_when_two_daemons_match(self):
        with mock.patch.object(SYS, "daemon_pids", lambda: [1, 2]), mock.patch.object(SYS, "stop_pids") as stop:
            _, _, job = self.go("daemon.restart", {}, True)
        self.assertEqual(job["state"], "failed")
        stop.assert_not_called()

    def test_start_daemon_runs_live_in_its_checkout(self):
        with mock.patch.object(dashboard_proc, "spawn_detached", lambda cmd, cwd, log: (self.fake.calls.append(cmd), 1)[1]):
            SYS.start_daemon()
        self.assertEqual(self.fake.calls[0][-2:], ["run", "--live"])

    def test_update_refuses_a_dirty_checkout_and_never_restarts(self):
        self.fake.out = " M tools/x.py"
        with mock.patch.object(SYS, "_restart") as rs:
            _, _, job = self.go("daemon.update", {}, True)
        self.assertEqual(job["state"], "failed")
        rs.assert_not_called()

    def test_update_fast_forwards_then_restarts(self):
        outs = iter([(0, ""), (0, "fetched"), (0, "Fast-forward")])
        with mock.patch.object(dashboard_proc, "run_cmd", lambda c, t, cwd=None: (self.fake.calls.append(c), next(outs))[1]), \
                mock.patch.object(SYS, "_restart", lambda p, t: (True, "restarted")):
            _, _, job = self.go("daemon.update", {}, True)
        self.assertEqual(job["output"], "restarted")
        self.assertIn("--ff-only", self.fake.calls[2])

    def test_supervisor_needs_its_script(self):
        self.assertEqual(A.submit("supervisor.start", {})[0], 400)     # the temp checkout has none
        os.makedirs(os.path.join(self.tmp, "tools"))
        open(os.path.join(self.tmp, "tools", "supervisor.py"), "w").close()
        with mock.patch.object(SYS, "supervisor_pids", lambda: [3]):
            _, _, job = self.go("supervisor.start", {})
        self.assertEqual(job["state"], "failed")                        # already running

    def test_disk_preview_is_the_dry_run_and_clean_is_confirmed(self):
        _, _, job = self.go("disk.preview", {})
        self.assertEqual(job["state"], "done")
        self.assertIn("--dry-run", self.fake.calls[0])
        self.assertTrue(self.fake.calls[0][1].replace("\\", "/").endswith("tools/clean.sh"))
        self.assertEqual(A.submit("disk.clean", {})[0], 409)
        self.assertEqual(A.submit("disk.clean", {"mode": "rm -rf"}, True)[0], 400)
        _, _, job = self.go("disk.clean", {"mode": "worktrees"}, True)
        self.assertNotIn("--dry-run", self.fake.calls[-1])
        self.assertIn("--worktrees-only", self.fake.calls[-1])


class Served(Base):
    def setUp(self):
        super().setUp()
        srv = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.Handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        self.base = "http://127.0.0.1:%d" % srv.server_address[1]

    def post(self, path, obj, token=None, ctype="application/json"):
        h = {"Content-Type": ctype}
        if token is not None:
            h[H.TOKEN_HEADER] = token
        req = urllib.request.Request(self.base + path, json.dumps(obj).encode(), h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=5) as r:
            return r.read()

    def test_every_post_needs_the_token(self):
        body = {"action": "daemon.status", "params": {}}
        self.assertEqual(self.post("/api/action", body)[0], 403)
        self.assertEqual(self.post("/api/action", body, "wrong")[0], 403)
        self.assertEqual(self.post("/api/agent/ask", {"name": "x", "text": "y"})[0], 403)   # the chat too
        self.assertEqual(self.post("/api/action", body, H.TOKEN, "text/plain")[0], 415)
        self.assertEqual(self.post("/api/action", body, H.TOKEN)[0], 202)

    def test_page_carries_the_token_and_actions_endpoints_work(self):
        page = self.get("/")
        self.assertIn(H.TOKEN.encode(), page)
        self.assertNotIn(H.PLACEHOLDER, page)
        self.assertIn(b"/static/dash-actions.js", page)
        view = json.loads(self.get("/api/actions"))
        self.assertIn("daemon.restart", {a["name"] for a in view["actions"]})
        self.assertNotIn(H.TOKEN, json.dumps(view))
        code, body = self.post("/api/action", {"action": "daemon.restart", "params": {}}, H.TOKEN)
        self.assertEqual((code, body.get("needs_confirm")), (409, True))

    def test_job_output_is_readable_over_http(self):
        code, body = self.post("/api/action", {"action": "disk.preview", "params": {}}, H.TOKEN)
        self.assertEqual(code, 202)
        for _ in range(50):
            j = json.loads(self.get("/api/action?id=" + body["job"]))
            if j["state"] != "running":
                break
            threading.Event().wait(0.1)
        self.assertEqual((j["state"], j["output"]), ("done", "fake output"))


if __name__ == "__main__":
    unittest.main()

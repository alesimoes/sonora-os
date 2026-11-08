"""Dashboard clarity: run rows say what the agent does; pools resume in bulk
and the paused list is live."""
import json
import os
import shutil
import tempfile
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)

import dashboard_actions as A
import dashboard_actions_http  # noqa: F401  (registers the actions)
import dashboard_actions_pools as P
import dashboard_proc
import dashboard_runview as RV
from harness import run_view, status


class Title(unittest.TestCase):
    def test_first_meaningful_line(self):
        b = "\n\n## Consertar o main vermelho: nightly 41\n- detalhe\n"
        self.assertEqual(run_view.brief_title(b), "Consertar o main vermelho: nightly 41")

    def test_fallback_and_cap(self):
        self.assertEqual(run_view.brief_title("  \n", "main-red-1"), "main-red-1")
        self.assertEqual(len(run_view.brief_title("x" * 900)), run_view.TITLE_MAX)

    def test_run_view_without_task(self):
        v = run_view.run_view({"stream_path": "s"}, None)
        self.assertEqual((v["title"], v["pr"], v["stream_path"]), ("", None, "s"))

    def test_status_runs_carry_title_agent_branch_pr(self):
        class Store:
            def tasks(self, states=None, kind=None): return [{"id": "t1", "brief": "Fazer X\nmais", "kind": "fix", "agent": "fixer",
                                      "branch": "b", "pr": 7, "state": "building", "track": None,
                                      "created": 0, "attempts": 0}]
            def features(self): return []
            def reservations(self): return []
        class Cfg:
            floors, cap, max_open_prs, max_ready_unpr = {}, 3, 4, 2
        run = {"id": "r", "task_id": "t1", "role": "fix", "name": "n", "pid": 1, "started": 1,
               "last_beat": 2, "working": True, "building": False, "suspect": 0, "stream_path": "/x"}
        d = status.build(Store(), Cfg, 10, [run], {}, "live", False, False, None, set(), 0)
        r = d["runs"][0]
        self.assertEqual((r["title"], r["kind"], r["agent"], r["branch"], r["pr"]), ("Fazer X", "fix", "fixer", "b", 7))


class Env(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.odir = os.path.join(self.tmp, "planning", "orchestrator")
        os.makedirs(self.odir)
        env = mock.patch.dict(os.environ, {"SONORA_ORCH_STATUS": os.path.join(self.odir, "status.json"),
                                           "SONORA_DASH_AUDIT": os.path.join(self.tmp, "audit.log")})
        env.start()
        self.addCleanup(env.stop)

    def write(self, name, obj):
        with open(os.path.join(self.odir, name), "w", encoding="utf-8") as f:
            json.dump(obj, f)


class LivePaused(Env):
    def test_overlay_replaces_stale_status(self):
        self.write("paused.json", ["plan"])
        o = {"paused": ["plan", "build"], "pools": {"plan": {}, "build": {"paused": True}}}
        v = RV.with_live_paused(o)
        self.assertEqual(v["paused"], ["plan"])
        self.assertFalse(v["pools"]["build"]["paused"])
        self.assertTrue(v["pools"]["plan"]["paused"])
        self.assertEqual(o["paused"], ["plan", "build"])   # input untouched

    def test_missing_file_means_nothing_paused_and_garbage_keeps_status(self):
        self.assertEqual(RV.with_live_paused({"paused": ["x"]})["paused"], [])
        with open(os.path.join(self.odir, "paused.json"), "w") as f:
            f.write("{oops")
        self.assertEqual(RV.with_live_paused({"paused": ["x"]})["paused"], ["x"])

    def test_last_step_from_stream(self):
        p = os.path.join(self.tmp, "stream.log")
        with open(p, "w", encoding="utf-8") as f:
            f.write(json.dumps({"type": "assistant", "message": {"content": [
                {"type": "tool_use", "name": "Bash", "input": {"command": "ls"}}]}}) + "\n")
        self.assertEqual(RV.last_step(p), "Bash: ls")
        self.assertEqual(RV.last_step(os.path.join(self.tmp, "none")), "")
        out = RV.add_last_steps({"runs": [{"stream_path": p}]})
        self.assertEqual(out["runs"][0]["last_step"], "Bash: ls")


class ResumeGroup(Env):
    def setUp(self):
        super().setUp()
        self.calls = []
        def fake(cmd, timeout, cwd=None):
            self.calls.append(cmd)
            return 0, "ok"
        p = mock.patch.object(dashboard_proc, "run_cmd", fake)
        p.start()
        self.addCleanup(p.stop)
        A.JOBS.clear()
        self.write("status.json", {"pools": {}, "paused": []})

    def go(self, group):
        code, body = A.submit("pool.resume_group", {"group": group}, wait=True)
        return code, (A.job_view(body["job"]) if code == 202 else body)

    def test_pools_of(self):
        paused = ["plan", "build:os", "build", "qa"]
        self.assertEqual(P.pools_of("build", paused), ["build", "build:os"])
        self.assertEqual(P.pools_of("all", paused), ["build", "build:os", "plan", "qa"])

    def test_build_group_resumes_only_paused_build_pools(self):
        self.write("paused.json", ["plan", "build", "build:daw", "build:os", "build:plugin"])
        code, job = self.go("build")
        self.assertEqual((code, job["state"]), (202, "done"))
        pools = [c[c.index("--pool") + 1] for c in self.calls]
        self.assertEqual(pools, ["build", "build:daw", "build:os", "build:plugin"])
        self.assertTrue(all(c[-1] == "--resume" for c in self.calls))

    def test_all_group_and_empty(self):
        self.write("paused.json", ["plan", "qa"])
        self.go("all")
        self.assertEqual(len(self.calls), 2)
        self.calls.clear()
        self.write("paused.json", [])
        _, job = self.go("all")
        self.assertEqual(self.calls, [])
        self.assertEqual(job["state"], "done")

    def test_bad_group_refused(self):
        self.assertEqual(self.go("rm -rf")[0], 400)


class Frontend(unittest.TestCase):
    def read(self, n):
        with open(os.path.join(os.path.dirname(__file__), "..", "..", "dashboard-static", n), encoding="utf-8") as f:
            return f.read()

    def test_pause_box_has_bulk_buttons_and_hints(self):
        s = self.read("dash-strip.js")
        self.assertIn("Retomar toda a construção", s)
        self.assertIn("Retomar tudo", s)
        self.assertIn("tarefas sem trilha definida", self.read("dash-tech.js"))

    def test_action_refreshes_state_at_once(self):
        self.assertIn("tick()", self.read("dash-actions.js"))

    def test_run_row_shows_title_and_last_step(self):
        s = self.read("dash-agents.js")
        self.assertIn("r.title", s)
        self.assertIn("r.last_step", s)


if __name__ == "__main__":
    unittest.main()

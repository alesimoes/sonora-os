"""The dashboard's disk source and the pieces split out of dashboard.py:
docker df parsing, free-space thresholds, the slow background sources, the
repo scan, the static route and the page's file references."""
import json
import os
import re
import shutil
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)

import dashboard_bg
import dashboard_disk
import dashboard_diskscan
import dashboard_static
import dashboard_util

GIB = 1024 ** 3

DF = "\n".join(json.dumps(d) for d in (
    {"Type": "Images", "TotalCount": "12", "Active": "3", "Size": "10.5GB", "Reclaimable": "4GB (38%)"},
    {"Type": "Containers", "TotalCount": "5", "Active": "1", "Size": "1.2MB", "Reclaimable": "900kB (75%)"},
    {"Type": "Local Volumes", "TotalCount": "2", "Active": "2", "Size": "0B", "Reclaimable": "0B (0%)"},
    {"Type": "Build Cache", "TotalCount": "80", "Active": "0", "Size": "22.4GB", "Reclaimable": "22.4GB"},
))


class Thresholds(unittest.TestCase):
    def test_levels_at_the_edges(self):
        self.assertEqual(dashboard_disk.level(30 * GIB), "ok")
        self.assertEqual(dashboard_disk.level(30 * GIB - 1), "warn")
        self.assertEqual(dashboard_disk.level(10 * GIB), "warn")
        self.assertEqual(dashboard_disk.level(10 * GIB - 1), "crit")
        self.assertEqual(dashboard_disk.level(0), "crit")

    def test_read_drive_of_a_real_folder(self):
        d = dashboard_disk.read_drive(tempfile.gettempdir())
        self.assertTrue(d["ok"])
        self.assertGreaterEqual(d["total"], d["free"])
        self.assertIn(d["level"], ("ok", "warn", "crit"))


class DockerDf(unittest.TestCase):
    def test_sizes(self):
        p = dashboard_disk.parse_size
        self.assertEqual((p("10.5GB"), p("1.2MB"), p("900kB"), p("0B")), (10_500_000_000, 1_200_000, 900_000, 0))
        self.assertEqual((p(""), p(None), p("lots")), (-1, -1, -1))

    def test_rows(self):
        rows = {r["type"]: r for r in dashboard_disk.parse_docker_df(DF)}
        self.assertEqual(list(rows), ["Images", "Containers", "Local Volumes", "Build Cache"])
        self.assertEqual((rows["Images"]["count"], rows["Images"]["active"]), (12, 3))
        self.assertEqual((rows["Images"]["size"], rows["Images"]["reclaimable"]), (10_500_000_000, 4_000_000_000))
        self.assertEqual(rows["Build Cache"]["reclaimable"], 22_400_000_000)

    def test_garbage_and_docker_down_give_no_rows(self):
        self.assertEqual(dashboard_disk.parse_docker_df(""), [])
        self.assertEqual(dashboard_disk.parse_docker_df("Cannot connect to the Docker daemon\n[1]\n{}"), [])

    def test_docker_down_does_not_break_the_view(self):
        orig = dashboard_disk._docker.fn
        dashboard_disk._docker.fn = lambda: None
        dashboard_disk._docker._value, dashboard_disk._docker._at = None, 0.0
        dashboard_disk._docker._started = True      # no thread: refresh by hand
        self.addCleanup(setattr, dashboard_disk._docker, "fn", orig)
        dashboard_disk._docker.refresh()
        dashboard_disk._scan._started = True
        view = dashboard_disk.disk_view()
        self.assertFalse(view["docker"]["ok"])
        self.assertTrue(view["drive"]["ok"])
        json.dumps(view)


class Periodic(unittest.TestCase):
    def test_keeps_the_last_good_value_when_the_source_fails(self):
        answers = [{"ok": True, "n": 1}, None, RuntimeError("down")]

        def fn():
            a = answers.pop(0)
            if isinstance(a, Exception):
                raise a
            return a
        p = dashboard_bg.Periodic(fn, 60)
        for _ in range(3):
            p.refresh()
        self.assertEqual(p._value, {"ok": True, "n": 1})
        self.assertIsNotNone(p.age())

    def test_error_value_before_any_good_read(self):
        def boom():
            raise RuntimeError("no docker")
        p = dashboard_bg.Periodic(boom, 60)
        p.refresh()
        self.assertEqual(p._value, {"ok": False, "error": "no docker"})
        self.assertIsNone(p.age())

    def test_value_starts_one_thread_only(self):
        calls = []
        p = dashboard_bg.Periodic(lambda: calls.append(1) or {"ok": True}, 3600)
        p.value()
        p.value()
        for _ in range(100):
            if calls:
                break
            import time
            time.sleep(0.02)
        self.assertEqual(len(calls), 1)


class RepoScan(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def put(self, rel, n):
        path = os.path.join(self.tmp, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"x" * n)

    def test_counts_worktrees_and_build_dirs(self):
        self.put(".claude/worktrees/agent-a/src/a.txt", 100)
        self.put(".claude/worktrees/agent-a/b.txt", 50)
        self.put(".claude/worktrees/agent-b/c.txt", 400)
        self.put("build-asan/obj/x.o", 1000)
        self.put("build/y.o", 10)
        self.put("engine/not-counted.cpp", 7)
        got = dashboard_diskscan.scan_repo(self.tmp)
        self.assertEqual((got["worktrees"]["count"], got["worktrees"]["size"]), (2, 550))
        self.assertEqual([x["name"] for x in got["worktrees"]["top"]], ["agent-b", "agent-a"])
        self.assertEqual((got["builds"]["count"], got["builds"]["size"]), (2, 1010))
        self.assertEqual(got["builds"]["top"][0]["name"], "build-asan")
        self.assertFalse(got["partial"])

    def test_empty_repo_and_a_spent_budget(self):
        got = dashboard_diskscan.scan_repo(self.tmp)
        self.assertEqual((got["worktrees"]["count"], got["builds"]["count"]), (0, 0))
        self.put("build-x/a", 5)
        self.assertTrue(dashboard_diskscan.scan_repo(self.tmp, budget=-1)["partial"])


class Roots(unittest.TestCase):
    def test_main_root_climbs_out_of_a_worktree(self):
        self.assertEqual(dashboard_util.main_root("/r/sound/.claude/worktrees/agent-x"), os.path.abspath("/r/sound").replace("\\", "/"))
        self.assertEqual(dashboard_util.main_root("/r/sound"), os.path.abspath("/r/sound"))

    def test_orch_root_follows_the_status_path(self):
        old = os.environ.get("SONORA_ORCH_STATUS")
        self.addCleanup(lambda: os.environ.pop("SONORA_ORCH_STATUS", None) if old is None
                        else os.environ.update(SONORA_ORCH_STATUS=old))
        os.environ["SONORA_ORCH_STATUS"] = os.path.join("a", "b", "planning", "orchestrator", "status.json")
        self.assertEqual(dashboard_util.orch_root(), os.path.abspath(os.path.join("a", "b")))


class StaticFiles(unittest.TestCase):
    def test_serves_known_files_and_refuses_escapes(self):
        body, ctype = dashboard_static.static_file("/static/dash-disk.js")
        self.assertIn(b"renderDisk", body)
        self.assertEqual(ctype, "application/javascript; charset=utf-8")
        self.assertEqual(dashboard_static.static_file("/static/dash-base.css")[1], "text/css; charset=utf-8")
        self.assertEqual(dashboard_static.static_file("/static/dash-logo.svg")[1], "image/svg+xml")
        for bad in ("/static/../dashboard.py", "/static/..%2fdashboard.py", "/static/nope.js", "/static/"):
            self.assertIsNone(dashboard_static.static_file(bad), bad)

    def test_every_file_the_page_links_exists_and_files_stay_small(self):
        html = dashboard_static.page()[0].decode("utf-8")
        refs = re.findall(r'(?:src|href)="(/static/[^"]+)"', html)
        self.assertGreater(len(refs), 10)
        for r in refs:
            self.assertIsNotNone(dashboard_static.static_file(r), r)
        for n in os.listdir(dashboard_static.STATIC_DIR):
            with open(os.path.join(dashboard_static.STATIC_DIR, n), encoding="utf-8") as f:
                self.assertLess(len(f.read().splitlines()), 500, n)
        self.assertIn('id="disk"', html)

    def test_no_file_defines_a_global_twice(self):
        names = {}
        for n in sorted(os.listdir(dashboard_static.STATIC_DIR)):
            if not n.endswith(".js"):
                continue
            with open(os.path.join(dashboard_static.STATIC_DIR, n), encoding="utf-8") as f:
                for m in re.finditer(r"^(?:const|let|function|async function) ([A-Za-z_$][\w$]*)", f.read(), re.M):
                    self.assertNotIn(m.group(1), names, "%s in %s and %s" % (m.group(1), n, names.get(m.group(1))))
                    names[m.group(1)] = n


class Served(unittest.TestCase):
    def test_page_static_and_state_over_http(self):
        import threading
        import urllib.request
        from http.server import ThreadingHTTPServer
        import dashboard
        import dashboard_state
        dashboard_state.state["disk"] = {"drive": {"ok": True, "level": "warn"}}
        srv = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.Handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        base = "http://127.0.0.1:%d" % srv.server_address[1]

        def get(path):
            try:
                with urllib.request.urlopen(base + path, timeout=5) as r:
                    return r.status, r.read()
            except urllib.error.HTTPError as e:
                return e.code, b""
        self.assertIn(b"/static/dash-main.js", get("/")[1])
        self.assertEqual(get("/static/dash-core.js")[0], 200)
        self.assertEqual(get("/static/missing.js")[0], 404)
        self.assertEqual(json.loads(get("/api/state")[1])["disk"]["drive"]["level"], "warn")


class ReadPrsDraft(unittest.TestCase):
    def test_draft_flag_and_base_are_passed_on(self):
        import dashboard_github
        raw = json.dumps([{"number": 7, "title": "t", "headRefName": "b", "baseRefName": "main",
                           "mergeStateStatus": "CLEAN", "mergeable": "MERGEABLE", "createdAt": "x",
                           "isDraft": True, "statusCheckRollup": []}])
        orig = dashboard_github.run
        dashboard_github.run = lambda *a, **k: raw
        try:
            p = dashboard_github.read_prs()[0]
        finally:
            dashboard_github.run = orig
        self.assertTrue(p["draft"])
        self.assertEqual(p["base"], "main")


class PageUsesGithubStatus(unittest.TestCase):
    """The flow no longer reads local-ci lots or stacked-PR phases."""
    def test_static_has_no_gate_phases(self):
        d = os.path.join(os.path.dirname(dashboard_static.__file__), "dashboard-static")
        text = "".join(open(os.path.join(d, n), encoding="utf-8").read() for n in ("dash-flow.js", "dash-prs.js"))
        for word in ("empilhado", "retarget", "buildtime", "na fila do lote"):
            self.assertNotIn(word, text)


if __name__ == "__main__":
    unittest.main()

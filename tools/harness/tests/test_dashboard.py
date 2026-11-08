"""The dashboard's plumbing: roots, the static route, the page's file
references and the PR reader. The console itself is tested in
test_dashboard_console.py and test_console_*.py."""
import json
import os
import re
import unittest

import support  # noqa: F401  (sets sys.path)

import dashboard_static
import dashboard_util


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
        body, ctype = dashboard_static.static_file("/static/console-core.js")
        self.assertIn(b"flushQueue", body)
        self.assertEqual(ctype, "application/javascript; charset=utf-8")
        self.assertEqual(dashboard_static.static_file("/static/console.css")[1], "text/css; charset=utf-8")
        self.assertEqual(dashboard_static.static_file("/static/dash-logo.svg")[1], "image/svg+xml")
        for bad in ("/static/../dashboard.py", "/static/..%2fdashboard.py", "/static/nope.js", "/static/"):
            self.assertIsNone(dashboard_static.static_file(bad), bad)

    def test_every_file_the_page_links_exists_and_files_stay_small(self):
        html = dashboard_static.page()[0].decode("utf-8")
        refs = re.findall(r'(?:src|href)="(/static/[^"]+)"', html)
        self.assertGreaterEqual(len(refs), 5)
        for r in refs:
            self.assertIsNotNone(dashboard_static.static_file(r), r)
        for n in os.listdir(dashboard_static.STATIC_DIR):
            with open(os.path.join(dashboard_static.STATIC_DIR, n), encoding="utf-8") as f:
                self.assertLess(len(f.read().splitlines()), 500, n)

    def test_no_file_defines_a_global_twice(self):
        names = {}
        for n in sorted(os.listdir(dashboard_static.STATIC_DIR)):
            if not n.endswith(".js"):
                continue
            with open(os.path.join(dashboard_static.STATIC_DIR, n), encoding="utf-8") as f:
                for m in re.finditer(r"^(?:const|let|function|async function) ([A-Za-z_$][\w$]*)", f.read(), re.M):
                    self.assertNotIn(m.group(1), names, "%s in %s and %s" % (m.group(1), n, names.get(m.group(1))))
                    names[m.group(1)] = n

    def test_old_pages_are_gone_and_nothing_loads_the_network(self):
        names = os.listdir(dashboard_static.STATIC_DIR)
        self.assertFalse([n for n in names if n.startswith("dash-") and n != "dash-logo.svg"])
        html = dashboard_static.page()[0].decode("utf-8")
        self.assertNotIn("https://", html)


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


if __name__ == "__main__":
    unittest.main()

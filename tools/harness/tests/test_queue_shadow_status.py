"""AGENT-QUEUE import, shadow mode and the status.json the dashboard reads."""
import json
import os
import re
import time
import unittest

import support
import orchestrator
from harness import queueimport, shadow, status

QUEUE = """# Agent queue (V1)

Prose that mentions MIDI, virtual instruments, video and immersive.

| Order | Item | Agent | Waits on |
|---|---|---|---|
| 1 | plugins.stock (EQ, dynamics, reverb, delay) | dsp-engineer | CMake refactor |
| 2 | native.tuner (vocal tuner) | dsp-engineer | CMake refactor |
| 3 | mixassist.workflow (gain staging, buses) | architect, then rt-engineer | mixassist.plans contract |
| 4 | ui.mixer.groups, ui.mixer.vca, ui.mixer.folders | ui-engineer | consolidate control command |
| 5 | ui.mixer.io, ui.mixer.sidechain | ui-engineer | ui.mixer.groups |
| 6 | midi.import (SMF) | rt-engineer | - |
| 7 | video.sync | rt-engineer | - |
| 8 | surround.panner and immersive.bed | dsp-engineer | - |
| 9 | automation.list finished from #361, automation.write open parts | rt-engineer | build throttle |
| 10 | installer slice b+c (session carry-over) | distro-engineer | #353 merged |
| 11 | virtual instruments host | plugin-engineer | - |
"""


class QueueImport(unittest.TestCase):
    def test_parse_rows(self):
        rows = queueimport.parse(QUEUE)
        ids = [r["id"] for r in rows]
        self.assertEqual(ids, ["plugins.stock", "native.tuner",
                               "mixassist.workflow", "ui.mixer.groups",
                               "ui.mixer.io", "automation.list",
                               "installer-slice-b-c"])
        by = {r["id"]: r for r in rows}
        self.assertEqual(by["plugins.stock"]["track"], "plugin")
        self.assertEqual(by["mixassist.workflow"]["track"], "daw")
        self.assertEqual(by["installer-slice-b-c"]["track"], "os")
        self.assertEqual(by["ui.mixer.groups"]["backlog_ids"],
                         ["ui.mixer.groups", "ui.mixer.vca",
                          "ui.mixer.folders"])
        self.assertEqual(by["automation.list"]["backlog_ids"],
                         ["automation.list", "automation.write"])
        self.assertEqual(by["ui.mixer.io"]["waits_on"], "ui.mixer.groups")

    def test_v1_scope_filter(self):
        for text in ("midi.import", "video.sync", "immersive.bed",
                     "surround.panner", "virtual instruments host"):
            self.assertTrue(queueimport.out_of_scope(text), text)
        self.assertFalse(queueimport.out_of_scope("native.tuner"))


class QueueCli(support.DaemonCase):
    def test_import_queue_creates_features_and_is_idempotent(self):
        path = os.path.join(self.tmp, "AGENT-QUEUE.md")
        with open(path, "w", encoding="utf-8") as f:
            f.write(QUEUE)
        self.assertEqual(orchestrator.main(
            ["--root", self.tmp, "import-queue", path]), 0)
        self.assertEqual(orchestrator.main(
            ["--root", self.tmp, "import-queue", path, "--wants",
             "adr,protocol"]), 0)
        o = self.make()
        feats = o.store.features()
        self.assertEqual(len(feats), 7)
        self.assertEqual({f["state"] for f in feats}, {"queued"})
        self.assertEqual(o.flow.info("plugins.stock")["wants"], ["adr"])
        self.assertEqual(o.flow.info("plugins.stock")["backlog_ids"],
                         ["plugins.stock"])
        self.assertEqual(o.store.get_feature("plugins.stock")["track"],
                         "plugin")


class StatusJson(support.DaemonCase):
    """tools/dashboard.html (renderOrch) reads: pools with name/working/floor
    (object or list), features with name|title|id, pr, state, tasks|strip
    whose state text is matched by regex, waits_on_owner items with
    title|text, and a string `updated`."""
    DONE = re.compile(r"done|ok|merged|x|feito")
    BLOCK = re.compile(r"block|wait|fail|red")
    DOING = re.compile(r"doing|work|run|progress|building")

    def colour(self, state):
        s = state.lower()
        return "done" if self.DONE.search(s) else \
            "block" if self.BLOCK.search(s) else \
            "doing" if self.DOING.search(s) else ""

    def test_features_as_the_dashboard_reads_them(self):
        o = self.make(dry_run=True, floors={"daw": 3})
        o.store.add_feature("f1", "daw")
        for tid, st in (("a", "building"), ("b", "queued"),
                        ("c", "integrating"), ("d", "ci_fix"),
                        ("e", "bugfix"), ("g", "qa")):
            o.store.add_task(tid, "daw", "rt-engineer", feature_id="f1",
                             state=st, files=[f"{tid}/**"])
        o.store.set_feature("f1", pr=42)
        o.store.add_task("z", "daw", "rt-engineer", state="blocked")
        o.store.set_task("z", note="third bounce")
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        self.assertIsInstance(st["updated"], str)
        self.assertRegex(st["updated"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d$")
        self.assertGreater(st["updated_ts"], 0)
        feat = next(x for x in st["features"] if x["name"] == "f1")
        self.assertEqual((feat["pr"], feat["tasks_total"]), (42, 6))
        want = {"a": "doing", "b": "", "c": "done", "d": "block",
                "e": "doing", "g": "doing"}
        got = {t["name"]: self.colour(t["state"]) for t in feat["tasks"]}
        self.assertEqual(got, want)
        self.assertEqual({t["name"]: t["stage"] for t in feat["tasks"]}["d"],
                         "ci_fix")
        pool = st["pools"]["build:daw"]
        self.assertEqual(pool["floor"], 3)
        self.assertIn("working", pool)
        for p in st["pools"].values():
            self.assertIn("working", p)
            self.assertIn("floor", p)
        (wait,) = st["waits_on_owner"]
        self.assertIn("third bounce", wait["title"])
        self.assertEqual(wait["task"], "z")

    def test_lone_task_is_a_one_task_feature(self):
        o = self.make()
        o.store.add_task("solo", "daw", "rt-engineer")
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        feat = next(x for x in st["features"] if x["name"] == "solo")
        self.assertEqual(feat["tasks_total"], 1)

    def test_adr_and_blocked_reason_are_shown(self):
        o = self.make(dry_run=True)
        o.flow.open_feature("q", "daw", ["q.x"])
        o.store.set_feature("q", adr_no=51)
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            feat = json.load(f)["features"][0]
        self.assertEqual((feat["name"], feat["adr"], feat["tasks"]),
                         ("q", 51, []))


class Shadow(support.DaemonCase):
    def agents(self, rows):
        path = os.path.join(self.tmp, "agents.json")
        with open(path, "w") as f:
            json.dump({"updated": "now", "agents": rows}, f)
        return path

    def log(self, o):
        p = os.path.join(o.odir, "shadow.log")
        if not os.path.exists(p):
            return ""
        with open(p) as f:
            return f.read()

    def test_shadow_never_launches_and_logs_differences(self):
        path = self.agents([
            {"name": "ui-engineer-1", "role": "engineer", "task": "a",
             "track": "daw"},
            {"name": "mystery", "role": "engineer", "task": "zzz",
             "track": "daw"}])
        c = support.scheduler.Config(cap=20, floors={"daw": 3},
                                     backoff_s=(0, 0, 0), local_builds=3)
        o = orchestrator.Orchestrator(
            self.tmp, cfg=c, broker=support.StubBroker(), dry_run=False,
            fake_claude=support.FAKE, clock=self.clock,
            forge=support.FakeForge(), worktrees=support.DirWorktrees(self.tmp),
            seed_fn=lambda k: 10, shadow_agents=path)
        self.orchs.append(o)
        self.assertTrue(o.dry_run)       # --shadow implies dry run
        for tid in ("a", "b"):
            o.store.add_task(tid, "daw", "rt-engineer", files=[f"{tid}/**"])
        o.tick()
        o.tick()
        self.assertEqual(o.store.active_runs(), [])
        text = self.log(o)
        self.assertIn("DAEMON_ONLY would launch engineer for b", text)
        self.assertNotIn("for a ", text)            # a matches a live agent
        self.assertIn("LIVE_ONLY mystery", text)
        self.assertIn("POOL daw: coordinator has 2 engineers, floor 3", text)
        # unchanged picture: nothing more is written
        size = len(text)
        o.tick()
        self.assertEqual(len(self.log(o)), size)

    def test_agreement_is_logged_once(self):
        path = self.agents([{"name": "e1", "role": "engineer",
                             "task": "feat/x", "track": "daw"}])
        sh = shadow.Shadow(os.path.join(self.tmp, "s.log"), path)
        task = {"id": "x", "feature_id": "x", "branch": "feat/x",
                "backlog_id": None}
        ln = support.scheduler.Launch("x", "engineer", "build", 3)
        self.assertEqual(sh.record([task], [ln], {}), [])
        self.assertEqual(sh.record([task], [ln], {}), [])
        with open(os.path.join(self.tmp, "s.log")) as f:
            self.assertEqual(len(f.read().splitlines()), 1)

    def test_unreadable_agents_file_is_a_difference_not_a_crash(self):
        sh = shadow.Shadow(os.path.join(self.tmp, "s.log"),
                           os.path.join(self.tmp, "missing.json"))
        self.assertIn("unreadable", sh.compare([], [], {})[0])

    def test_bare_list_and_backlog_id_match(self):
        path = os.path.join(self.tmp, "l.json")
        with open(path, "w") as f:
            json.dump([{"name": "e", "task": "ui.mixer.groups"}], f)
        sh = shadow.Shadow(os.path.join(self.tmp, "s.log"), path)
        task = {"id": "t", "feature_id": "f", "branch": "feat/t",
                "backlog_id": "ui.mixer.vca,ui.mixer.groups"}
        ln = support.scheduler.Launch("t", "engineer", "build", 3)
        self.assertEqual(sh.compare([task], [ln], {}), [])

    def test_cli_needs_the_agents_file(self):
        with self.assertRaises(SystemExit):
            orchestrator.main(["--root", self.tmp, "run", "--shadow"])


if __name__ == "__main__":
    unittest.main()

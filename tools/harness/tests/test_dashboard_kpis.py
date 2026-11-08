"""The console's KPI reader (dashboard_kpis.py, dashboard_progress.py): the
progress tracks from a catalogue, the open-PR list, the merge counts and the
Progress poller that keeps the last good value."""
import json
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)
import dashboard_console as V
import dashboard_kpis as K
import dashboard_progress as P
import dashboard_state as ST

NOW = 1_800_000_000.0
SIZES = {"S": 1, "M": 3, "L": 8}


def item(i, size, status):
    return {"id": i, "title": i, "size": size, "phase": 1, "status": status}


def catalogue(optional=False):
    extra = [dict(item("x", "L", "todo"), optional=True)] if optional else []
    return {"sizes": SIZES, "post_launch_phases": [], "phases": {"1": "p"}, "fronts": [
        {"id": "daw", "title": "Complete DAW", "modules": [
            {"id": "daw.core", "title": "core", "items": [item("a", "L", "done"), item("b", "M", "doing")] + extra},
            {"id": "daw.native", "title": "native", "items": [item("c", "M", "done"), item("d", "M", "todo")]}]},
        {"id": "os", "title": "OS packaging", "modules": [
            {"id": "os.image", "title": "image", "items": [item("e", "S", "done"), item("f", "S", "todo")]}]}]}


def pr(n, gate, **kw):
    return dict({"number": n, "title": "t%d" % n, "gate": gate, "gate_text": gate,
                 "owner": "daemon", "opened_min": 12}, **kw)


class Progress(unittest.TestCase):
    def test_tracks_split_plugins_from_the_daw_and_add_up(self):
        v = K.progress_view(catalogue())
        by = {t["key"]: t for t in v["tracks"]}
        self.assertEqual((by["daw"]["done"], by["daw"]["doing"], by["daw"]["total"]), (8, 3, 11))
        self.assertEqual((by["plugins"]["done"], by["plugins"]["total"]), (3, 6))
        self.assertEqual((by["os"]["done"], by["os"]["total"]), (1, 2))
        self.assertEqual(sum(t["total"] for t in v["tracks"]), v["total"])
        self.assertEqual((v["done"], v["doing"], v["total"]), (12, 3, 19))
        self.assertEqual(v["pct"], 100 * 12 // 19)       # floored like progress.py
        self.assertEqual(by["os"]["pct"], 50)

    def test_optional_work_does_not_dilute(self):
        self.assertEqual(K.progress_view(catalogue(True))["total"], 19)

    def test_the_real_catalogue_reads(self):
        with open(P.__file__.replace("dashboard_progress.py", "../planning/progress.json"), encoding="utf-8") as f:
            v = K.progress_view(json.load(f))
        self.assertEqual([t["key"] for t in v["tracks"]], ["daw", "plugins", "os"])
        self.assertEqual(sum(t["total"] for t in v["tracks"]), v["total"])


class Prs(unittest.TestCase):
    def test_ci_words_levels_and_tap_question(self):
        rows = K.pr_list([pr(1, "verde"), pr(2, "vermelho"), pr(3, "rodando"), pr(4, "conflito"),
                          pr(5, "rascunho")], [], NOW)
        self.assertEqual([r["ci"] for r in rows], ["verde", "vermelho", "rodando", "vermelho", "rascunho"])
        self.assertEqual([r["level"] for r in rows], ["ok", "bad", "run", "bad", "warn"])
        self.assertEqual(rows[1]["tap"], "por que o PR 2 não entrou?")
        self.assertEqual(rows[0]["minutes_open"], 12)

    def test_train_phase_joins_the_owner_text_once(self):
        rows = K.pr_list([pr(7, "rodando"), pr(8, "verde", owner="daemon")],
                         [{"pr": 7, "phase": "esperando checks"}, {"pr": 8, "phase": "daemon"}])
        self.assertEqual(rows[0]["train"], "esperando checks · daemon")
        self.assertEqual(rows[1]["train"], "daemon")

    def test_empty_and_capped(self):
        self.assertEqual(K.pr_list(None, None), [])
        self.assertEqual(len(K.pr_list([pr(n, "verde") for n in range(100)], [])), K.MAX_PRS)


class Merges(unittest.TestCase):
    def iso(self, ago_s):
        import time
        return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(NOW - ago_s))

    def test_today_and_last_hour(self):
        m = [{"mergedAt": self.iso(60)}, {"mergedAt": self.iso(3500)}, {"mergedAt": self.iso(3700)},
             {"mergedAt": "garbage"}]
        self.assertEqual(K.merges_view(m, NOW), {"today": 3, "last_hour": 2})

    def test_none(self):
        self.assertEqual(K.merges_view(None, NOW), {"today": 0, "last_hour": 0})


class Wiring(unittest.TestCase):
    def test_console_view_carries_kpis(self):
        snap = {"orchestrator": {"updated_ts": NOW - 1, "train": []}, "progress": {"pct": 5},
                "board": {"agents": [], "prs": [pr(9, "verde")]}, "merged": []}
        out = V.build(snap, True, NOW)
        self.assertEqual(out["kpis"]["progress"], {"pct": 5})
        self.assertEqual(out["kpis"]["prs"][0]["number"], 9)
        self.assertEqual(out["kpis"]["merges"]["today"], 0)

    def test_empty_snapshot_does_not_raise(self):
        self.assertEqual(V.build({}, False, NOW)["kpis"]["prs"], [])


class Poller(unittest.TestCase):
    def test_keeps_last_good_value_and_lists_the_error(self):
        p, errors = ST.Progress(), []
        with mock.patch.object(ST, "read_progress", return_value={"pct": 3}):
            p.poll(errors)
        self.assertEqual(p.value, {"pct": 3})
        p.at = 0
        with mock.patch.object(ST, "read_progress", side_effect=ValueError("x")):
            p.poll(errors)
        self.assertEqual(p.value, {"pct": 3})
        self.assertEqual(len(errors), 1)

    def test_reads_at_most_every_interval(self):
        p = ST.Progress()
        with mock.patch.object(ST, "read_progress", return_value={"pct": 1}) as r:
            p.poll([]); p.poll([])
        self.assertEqual(r.call_count, 1)

    def test_reader_uses_origin_main(self):
        calls = []
        def fake(cmd, timeout=40):
            calls.append(cmd)
            return json.dumps(catalogue()) if cmd[3] == "show" else ""
        with mock.patch.object(P, "run", fake), mock.patch.object(P, "orch_root", lambda: "/o"):
            v = P.read_progress()
        self.assertEqual(v["total"], 19)
        self.assertIn("origin/main:planning/progress.json", calls[-1])

    def test_reader_none_when_unreadable(self):
        with mock.patch.object(P, "run", lambda *a, **k: ""), mock.patch.object(P, "orch_root", lambda: "/o"):
            self.assertIsNone(P.read_progress())


if __name__ == "__main__":
    unittest.main()

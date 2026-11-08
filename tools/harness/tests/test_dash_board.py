"""The dashboard board: every running agent from both launchers (duplicates
flagged), open PRs with gate and merge owner (green too long is red), the
brakes in plain words, and the hook that records the coordinator's subagents."""
import os
import shutil
import tempfile
import time
import unittest
from unittest import mock

import support  # noqa: F401  (sets sys.path)

import dashboard_board as B
import dashboard_brakes as K
import dashboard_orch
import dashboard_prboard as P
from harness import coord_agents as CA
from harness import coord_hook

NOW = 10_000.0


def run(name, role, task, pr=None, **kw):
    return dict({"id": "r-" + name, "name": name, "role": role, "task": task, "pr": pr,
                 "title": "Tarefa " + task, "started": NOW - 600, "last_beat": NOW - 20,
                 "last_step": "Bash: pytest", "building": False, "suspect": False}, **kw)


class Agents(unittest.TestCase):
    def test_both_launchers_listed_oldest_first_with_who_what_since_last(self):
        orch = {"runs": [run("principal-2", "principal", "pr-515", 515)],
                "features": [{"id": "feat-a", "pr": None, "tasks": [{"name": "t9"}]}]}
        coord = [{"id": "tu1", "role": "reviewer", "name": "reviewer", "doing": "Revisar o PR 600",
                  "pr": 600, "since": NOW - 900, "last": NOW - 5, "step": "Read: x"}]
        rows = B.agent_rows(orch, coord, [], NOW)
        self.assertEqual([r["launcher"] for r in rows], ["coordenador", "daemon"])
        d = rows[1]
        self.assertEqual((d["role_pt"], d["name"], d["what"], d["pr"], d["since"], d["last"], d["step"]),
                         ("Engenheiro principal", "principal-2", "Tarefa pr-515", 515, NOW - 600, NOW - 20, "Bash: pytest"))

    def test_daemon_run_gets_feature_and_pr_from_its_feature(self):
        orch = {"runs": [run("eng-1", "engineer", "t9")],
                "features": [{"id": "feat-a", "pr": 77, "tasks": [{"name": "t9"}]}]}
        r = B.agent_rows(orch, [], [], NOW)[0]
        self.assertEqual((r["feature"], r["pr"]), ("feat-a", 77))

    def test_principal_on_the_same_pr_from_both_launchers_is_flagged(self):
        orch = {"runs": [run("principal-2", "principal", "pr-515", 515)], "features": []}
        coord = [{"id": "tu1", "role": "principal-engineer", "name": "principal-engineer",
                  "doing": "Corrigir #515", "pr": 515, "since": NOW - 100, "last": NOW}]
        rows = B.agent_rows(orch, coord, [], NOW)
        self.assertTrue(all(r["dup"] for r in rows))
        self.assertIn("principal-2 (daemon)", rows[1]["dup_with"])

    def test_same_launcher_different_roles_on_a_pr_is_not_duplicate(self):
        orch = {"runs": [run("eng-1", "engineer", "a", 5), run("qa-1", "tester", "b", 5)], "features": []}
        self.assertFalse(any(r.get("dup") for r in B.agent_rows(orch, [], [], NOW)))

    def test_label_file_sessions_count_unless_done(self):
        labelled = [{"id": "x", "label": "Mixer", "agent": "ui-engineer", "doing": "UI", "pr": None,
                     "state": "working", "idle_seconds": 30},
                    {"id": "y", "label": "Old", "agent": "x", "state": "done"}]
        rows = B.agent_rows(None, [], labelled, NOW)
        self.assertEqual([(r["name"], r["last"], r["state"]) for r in rows], [("Mixer", NOW - 30, "trabalhando")])


def pr(n, **kw):
    return dict({"number": n, "title": "T", "created": "1970-01-01T00:00:00Z", "failed": [], "pending": [],
                 "passed": 3, "total": 3, "cancelled": [], "draft": False, "base": "main",
                 "merge": "CLEAN", "mergeable": "MERGEABLE"}, **kw)


class Prs(unittest.TestCase):
    def test_gate_states(self):
        self.assertEqual(P.gate(pr(1))[0], "verde")
        self.assertEqual(P.gate(pr(1, pending=["a"], passed=1))[0], "rodando")
        self.assertEqual(P.gate(pr(1, failed=["micro"]))[0], "vermelho")
        self.assertEqual(P.gate(pr(1, merge="DIRTY"))[0], "conflito")
        self.assertEqual(P.gate(pr(1, draft=True))[0], "rascunho")
        self.assertEqual(P.gate(pr(1, base="pr-9"))[0], "base")

    def test_merge_owner_by_state(self):
        self.assertIn("daemon", P.merge_owner("verde", None))
        self.assertIn("fixer-1", P.merge_owner("vermelho", {"fixer": "fixer-1"}))
        self.assertIn("falta corretor", P.merge_owner("vermelho", None))
        self.assertIn("integrador", P.merge_owner("conflito", None))

    def test_green_for_more_than_ten_minutes_is_late(self):
        clock = P.GreenClock()
        P.pr_rows([pr(1)], None, clock, NOW)
        early = P.pr_rows([pr(1)], None, clock, NOW + 9 * 60)[0]
        late = P.pr_rows([pr(1)], None, clock, NOW + 11 * 60)[0]
        self.assertFalse(early["late"])
        self.assertTrue(late["late"])
        self.assertEqual(late["green_min"], 11)
        self.assertIn("11 min", late["late_text"])

    def test_going_red_resets_the_green_clock(self):
        clock = P.GreenClock()
        P.pr_rows([pr(1)], None, clock, NOW)
        P.pr_rows([pr(1, failed=["x"])], None, clock, NOW + 700)
        again = P.pr_rows([pr(1)], None, clock, NOW + 710)[0]
        self.assertFalse(again["late"])

    def test_minutes_open(self):
        row = P.pr_rows([pr(1, created="1970-01-01T00:00:00Z")], None, P.GreenClock(), 90 * 60)[0]
        self.assertEqual(row["opened_min"], 90)


class Brakes(unittest.TestCase):
    O = {"updated_ts": NOW, "open_prs": 8, "max_open_prs": 8, "runs_last_hour": 12, "max_runs_per_hour": 60,
         "live": 3, "cap": 10, "paused": ["build:os"],
         "pools": {"build:daw": {"floor": 8, "live": 0, "reason": "8 open PRs: merging first"},
                   "build:os": {"floor": 4, "live": 0, "reason": "paused"},
                   "build:plugin": {"floor": 2, "live": 2, "reason": None}},
         "alerts": [{"id": "a", "level": "alert", "text": "fila de lock parada", "since": NOW - 1200, "check": "lock"}]}

    def texts(self, o=None, now=NOW):
        return [(r["level"], r["text"]) for r in K.brakes(self.O if o is None else o, now)]

    def test_pr_limit_in_plain_words(self):
        self.assertIn(("warn", "8 PRs abertos (limite 8): priorizando merges, nenhuma tarefa nova começa"), self.texts())

    def test_hour_and_cap_rows(self):
        t = [x[1] for x in self.texts()]
        self.assertIn("12 de 60 execuções na última hora", t)
        self.assertIn("3 de 10 agentes simultâneos", t)

    def test_hour_limit_reached(self):
        o = dict(self.O, runs_last_hour=60)
        self.assertTrue(any("limite 60" in x[1] and x[0] == "warn" for x in self.texts(o)))

    def test_every_held_build_pool_gives_its_reason(self):
        t = [x[1] for x in self.texts()]
        self.assertIn("Construção · DAW: 0 de 8 ativos, segurada porque: 8 PRs abertos: priorizando merges", t)
        self.assertIn("Construção · OS pausada", t)
        self.assertFalse(any("plugins" in x for x in t))

    def test_alerts_with_age(self):
        self.assertIn(("bad", "Alerta (lock): fila de lock parada · há 20 min"), self.texts())

    def test_switches_and_dead_daemon(self):
        o = dict(self.O, kill_switch="STOP", usage_pause_until=NOW + 600)
        t = [x[1] for x in self.texts(o)]
        self.assertTrue(any("Chave de emergência STOP" in x for x in t))
        self.assertTrue(any("Pausa por consumo da conta por mais 10 min" in x for x in t))
        self.assertEqual(K.brakes(None, NOW)[0]["level"], "bad")
        self.assertIn("parado há 10 min", K.brakes(self.O, NOW + 600)[0]["text"])


class Hook(unittest.TestCase):
    def ev(self, name, tool, inp=None, **kw):
        return dict({"hook_event_name": name, "tool_name": tool, "tool_input": inp or {}}, **kw)

    def test_launch_records_role_description_and_pr(self):
        a = CA.apply_event({}, self.ev("PreToolUse", "Agent", {"subagent_type": "principal-engineer",
                           "description": "Corrigir o PR #515", "run_in_background": True}, tool_use_id="tu1"), NOW)
        r = a["tu1"]
        self.assertEqual((r["role"], r["doing"], r["pr"], r["since"], r["background"]),
                         ("principal-engineer", "Corrigir o PR #515", 515, NOW, True))

    def test_foreground_run_leaves_when_the_tool_returns(self):
        pre = self.ev("PreToolUse", "Agent", {"subagent_type": "x", "description": "d"}, tool_use_id="t")
        a = CA.apply_event({}, pre, NOW)
        a = CA.apply_event(a, dict(pre, hook_event_name="PostToolUse"), NOW + 1)
        self.assertEqual(a, {})

    def test_background_run_stays_until_subagent_stop(self):
        pre = self.ev("PreToolUse", "Agent", {"subagent_type": "x", "description": "d", "run_in_background": True}, tool_use_id="t")
        a = CA.apply_event({}, pre, NOW)
        a = CA.apply_event(a, dict(pre, hook_event_name="PostToolUse", tool_response="agentId: ab12"), NOW + 1)
        self.assertEqual(a["t"]["agent_id"], "ab12")
        a = CA.apply_event(a, self.ev("SubagentStop", None, agent_id="ab12"), NOW + 50)
        self.assertEqual(a, {})

    def test_subagent_tool_calls_are_its_heartbeat_and_last_step(self):
        pre = self.ev("PreToolUse", "Agent", {"subagent_type": "reviewer", "description": "d", "run_in_background": True}, tool_use_id="t")
        a = CA.apply_event({}, pre, NOW)
        a = CA.apply_event(a, self.ev("PostToolUse", "Bash", {"command": "git diff"}, agent_id="zz", agent_type="reviewer"), NOW + 30)
        self.assertEqual((a["t"]["agent_id"], a["t"]["last"], a["t"]["step"]), ("zz", NOW + 30, "Bash: git diff"))

    def test_coordinator_own_tool_calls_do_not_touch_rows(self):
        a = {"t": {"id": "t", "role": "x", "since": NOW, "last": NOW, "agent_id": None}}
        self.assertEqual(CA.apply_event(a, self.ev("PostToolUse", "Bash", {"command": "ls"}), NOW + 5), a)

    def test_stale_rows_are_dropped(self):
        a = {"t": {"id": "t", "role": "x", "since": 0, "last": 0, "agent_id": None}}
        self.assertEqual(CA.apply_event(a, self.ev("PostToolUse", "Bash"), CA.STALE_S + 1), {})


class HookFile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_record_writes_the_file_the_dashboard_reads(self):
        base = os.path.join(self.tmp, ".harness")
        path = coord_hook.target({"SONORA_COORD_DIR": base})
        now = time.time()
        coord_hook.record({"hook_event_name": "PreToolUse", "tool_name": "Agent", "tool_use_id": "t1",
                           "tool_input": {"subagent_type": "reviewer", "description": "Revisar #9"}}, path, now)
        self.assertFalse(os.path.exists(path + ".lock"))
        env = {"SONORA_ORCH_STATUS": os.path.join(self.tmp, "planning", "orchestrator", "status.json")}
        with mock.patch.dict(os.environ, env):
            rows = dashboard_orch.read_coordinator_agents(now)
        self.assertEqual([(r["role"], r["pr"]) for r in rows], [("reviewer", 9)])

    def test_bad_input_never_raises(self):
        with mock.patch("sys.stdin", mock.Mock(read=lambda *a: "not json")):
            self.assertEqual(coord_hook.main(), 0)


class Layout(unittest.TestCase):
    TOOLS = os.path.join(os.path.dirname(__file__), "..", "..")

    def read(self, *p):
        with open(os.path.join(self.TOOLS, *p), encoding="utf-8") as f:
            return f.read()

    def test_board_sections_and_no_leftovers(self):
        h = self.read("dashboard.html")
        for i in ("boardbrakes", "boardagents", "boardprs", "boardwarn"):
            self.assertIn('id="%s"' % i, h)
        self.assertNotIn("Espera o dono", h)
        self.assertNotIn("dash-agents.js", h)
        self.assertIn("dash-board.js", h)
        for f in os.listdir(os.path.join(self.TOOLS, "dashboard-static")):
            if f.endswith(".js"):
                self.assertNotIn("CI local", self.read("dashboard-static", f))
        self.assertNotIn("CI local", self.read("dashboard_machine.py"))


if __name__ == "__main__":
    unittest.main()

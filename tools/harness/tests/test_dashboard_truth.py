"""The console shows Docker down, idle pools, blocked groups and supervisor
actions from status.json; and names the right blocker."""
import unittest

import support  # noqa: F401
import dashboard_attention as A
import dashboard_console as V
import dashboard_truth as T

NOW = 100000.0
O = {"updated_ts": NOW - 5, "docker_down_since": NOW - 600, "docker_held": ["t1"],
     "pools": {"build:daw": {"floor": 8, "working": 0, "docker_down": True, "paused": False},
               "build:os": {"floor": 4, "working": 0, "paused": False, "reason": "no eligible task"},
               "fix": {"floor": 0, "working": 0}},
     "blocked": [{"cause": "plan", "items": [{"task": "a", "note": "n" * 500}]},
                 {"cause": "code", "items": [{"task": "b", "note": "x"}, {"branch": "c", "note": "y"}]},
                 {"cause": "weird", "items": []}],
     "supervisor": {"actions": [{"what": "sent", "check": "notify", "reason": "r", "ts": 1}]},
     "waits_on_owner": [{"task": "ui.pan", "title": "third bounce"}]}


class Truth(unittest.TestCase):
    def test_docker_is_its_own_reason(self):
        t = T.build(O, NOW)
        self.assertEqual(t["docker"]["since_min"], 10)
        self.assertEqual(t["docker"]["held"], ["t1"])
        self.assertEqual([(p["pool"], p["why"]) for p in t["idle_pools"]],
                         [("build:daw", "docker"), ("build:os", "fila")])

    def test_no_docker_when_up(self):
        self.assertIsNone(T.build({"docker_down_since": None}, NOW)["docker"])

    def test_blocked_grouped_by_cause_biggest_first_with_trimmed_note(self):
        b = T.build(O, NOW)["blocked"]
        self.assertEqual([(g["cause"], g["n"]) for g in b], [("code", 2), ("plan", 1)])
        self.assertEqual(len(b[1]["items"][0]["note"]), T.NOTE_MAX)

    def test_supervisor_actions_listed(self):
        self.assertEqual(T.build(O, NOW)["supervisor"][0]["check"], "notify")

    def test_console_payload_has_truth_and_owner_item(self):
        d = V.build({"orchestrator": O, "board": {"agents": [], "prs": []}}, True, NOW)
        self.assertIn("docker", d["truth"])
        self.assertEqual(d["attention"][0]["title"], "ui.pan")


class Blocker(unittest.TestCase):
    def test_waits_for_wins_over_the_word_after_blocked(self):
        self.assertEqual(A.blocker_of("t sits idle: blocked: Wrote plan, waits for ui.mixer.pan"), "ui.mixer.pan")

    def test_blocked_task_id_still_works(self):
        self.assertIsNone(A.blocker_of("stuck: blocked sibling t0"))
        self.assertEqual(A.blocker_of("blocked: feat.a"), "feat.a")


if __name__ == "__main__":
    unittest.main()

"""'Por que não entrou?' (harness/whynot.py): one case per
way a green PR can sit unmerged, from the three seen on 2026-11-08."""
import json
import os
import tempfile
import unittest

import support  # noqa: F401
from harness import whynot


def pr(n=1, **kw):
    d = {"number": n, "branch": "b%d" % n, "base": "main", "merge": "CLEAN",
         "mergeable": "MERGEABLE", "failed": [], "pending": [], "cancelled": [],
         "passed": 3, "total": 3, "draft": False}
    d.update(kw)
    return d


def row(n=1, **kw):
    d = {"pr": n, "phase": "verde", "fixer": None, "passed": 3, "total": 3}
    d.update(kw)
    return d


def why(p, status=None, events=(), up=True):
    return whynot.explain(p, status or {}, list(events), up)


class Explain(unittest.TestCase):
    def test_no_owner_is_the_519_case(self):
        w = why(pr(519))
        self.assertIn("ninguém", w["owner"])
        self.assertFalse(w["on_train"])
        self.assertIn("adota sozinho", w["reason"])
        self.assertEqual(w["merge_state"], "CLEAN")
        self.assertEqual(w["gate"], "checks 3/3")

    def test_no_owner_and_daemon_down(self):
        self.assertIn("daemon não está rodando",
                      why(pr(), up=False)["reason"])

    def test_draft_waits_for_the_agent(self):
        self.assertIn("rascunho", why(pr(draft=True))["reason"])

    def test_running_principal_owns_it_527(self):
        st = {"features": [{"id": "f", "pr": 527, "state": "pr_open",
                            "tasks": [{"name": "f.1"}]}],
              "tasks": [{"id": "f.1", "state": "ci_fix", "pr": 527,
                         "branch": "x"}],
              "runs": [{"task": "f.1", "name": "principal-3",
                        "role": "principal"}]}
        w = why(pr(527), st)
        self.assertIn("principal-3", w["owner"])
        self.assertIn("ainda é dono", w["reason"])

    def test_red_without_fixer_is_the_528_case(self):
        st = {"train": [row(528, phase="CI vermelho")]}
        w = why(pr(528, failed=["build"], passed=2, total=3), st)
        self.assertIn("mande fixer", w["reason"])
        self.assertTrue(w["on_train"])
        self.assertIn("falhando: build", w["gate"])

    def test_red_with_fixer_running(self):
        st = {"train": [row(phase="x", fixer="fixer-2")],
              "runs": [{"task": "t", "name": "fixer-2", "role": "fixer"}]}
        # a fixer on the train row is working: the merge waits for it
        w = why(pr(failed=["build"]), st)
        self.assertIn("fixer-2", w["owner"])

    def test_conflict_without_agent(self):
        w = why(pr(merge="DIRTY", mergeable="CONFLICTING"),
                {"train": [row()]})
        self.assertIn("integrator", w["reason"])

    def test_pending_checks(self):
        w = why(pr(pending=["a", "b"], passed=1, total=3),
                {"train": [row(phase="CI rodando")]})
        self.assertIn("esperando 2", w["reason"])

    def test_green_clean_on_the_train_merges_next_cycle(self):
        w = why(pr(), {"train": [row()]})
        self.assertIn("faz o merge", w["reason"])
        self.assertIn("trem", w["owner"])

    def test_green_but_not_adopted_feature_only(self):
        st = {"features": [{"id": "f", "pr": 1, "state": "pr_open"}]}
        self.assertIn("feature f", why(pr(), st)["owner"])

    def test_task_with_the_branch_but_no_pr(self):
        st = {"tasks": [{"id": "t9", "state": "blocked", "pr": None,
                         "branch": "b1"}]}
        w = why(pr(), st)
        self.assertIn("tarefa t9", w["owner"])

    def test_last_event_by_task_or_number(self):
        ev = [{"ts": 5, "task_id": None, "kind": "other", "detail": "#2 x"},
              {"ts": 4, "task_id": "pr-1", "kind": "train_rerun",
               "detail": "again"},
              {"ts": 3, "task_id": None, "kind": "old", "detail": "#1 old"}]
        w = why(pr(1), {"train": [row()]}, ev)
        self.assertEqual(w["event"]["kind"], "train_rerun")
        self.assertIsNone(why(pr(7), {}, ev)["event"])


if __name__ == "__main__":
    unittest.main()

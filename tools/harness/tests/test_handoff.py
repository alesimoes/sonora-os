"""harness/handoff: the next role's prompt carries the previous run's
result.json (2026-11-08 audit: the tester after the engineer, the
integrator after the testers and a relaunch got only brief and note)."""
import json
import os
import unittest

import support
from harness import handoff
import test_pr_lifecycle as lc
from test_pr_lifecycle import argv_log, state


def prompts(o):
    return [a[-1] for a in argv_log(o)]   # launcher.build_command: last


class Unit(support.DaemonCase):
    def result(self, o, tid, run_id, role, name, **data):
        d = os.path.join(self.tmp, run_id)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, "result.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"status": "done", "summary": "x", **data}, f)
        o.store.add_run(id=run_id, task_id=tid, role=role, name=name,
                        state="building", pid=1, started=1.0, last_beat=1.0,
                        result_path=path, stream_path="s", worktree=None)
        o.store.set_run(run_id, ended=2.0, outcome="finished")

    def test_brief_is_newest_result_per_task_bounded(self):
        o = self.make()
        o.store.add_task("a", "daw", "rt-engineer", feature_id="f")
        o.store.add_task("b", "daw", "rt-engineer", feature_id="f")
        self.assertEqual(handoff.brief(o.store, o.store.get_task("a")), "")
        self.result(o, "a", "a-r1", "engineer", "eng-1", summary="old")
        self.result(o, "a", "a-r2", "engineer", "eng-1", summary="new one",
                    commit="abc1234", unverified=["ctest -L ui"],
                    findings=[{"severity": "minor", "text": "nit"}])
        self.result(o, "b", "b-r1", "tester", "tester-1",
                    summary="s" * 5000)
        text = handoff.brief(o.store, o.store.get_task("a"),
                             o.store.feature_tasks("f"))
        self.assertIn("engineer eng-1 on a: done", text)
        self.assertIn("summary: new one", text)
        self.assertNotIn("old", text)
        self.assertIn("commit: abc1234", text)
        self.assertIn("unverified: ctest -L ui", text)
        self.assertIn('"severity": "minor"', text)
        self.assertIn("tester tester-1 on b", text)
        self.assertLessEqual(len(text), handoff.MAX_CHARS + len(handoff.HEAD)
                             + 1)


class Daemon(support.DaemonCase):
    def test_tester_and_integrator_prompts_carry_the_previous_result(self):
        o = self.make(floors={"daw": 1})
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"],
                         feature_id="fa")
        self.assertTrue(self.pump(o, lambda: state(o, "a") == "pr_open"))
        ps = [p for p in prompts(o) if "task: a " in p]   # not the lead wake
        self.assertEqual(len(ps), 3)   # engineer, tester, integrator
        self.assertNotIn("Handoff", ps[0])
        self.assertIn("Handoff from the previous run", ps[1])
        self.assertIn("engineer eng-1 on a: done", ps[1])
        self.assertIn("tester tester-1 on a: done", ps[2])


if __name__ == "__main__":
    unittest.main()

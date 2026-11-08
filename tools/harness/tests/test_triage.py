"""harness/triage.py: a blocked result for a harness reason is requeued,
closed or sent on by the daemon, never left blocked."""
import unittest

import support
from harness import triage


class Classify(unittest.TestCase):
    def test_classes(self):
        c = triage.classify
        self.assertEqual(c("engineer", "broker unreachable: WinError 10061"), "infra")
        self.assertEqual(c("engineer", "hit the 10-minute command limit waiting on tools/build-slot.sh"), "infra")
        self.assertEqual(c("engineer", "docker engine error: cannot connect to the Docker daemon"), "infra")
        self.assertEqual(c("engineer", "No commits between main and feat/x"), "nocommit")
        self.assertEqual(c("engineer", "accept passes; blocked: files outside its claim: a/b.cpp"), "scope")
        self.assertIsNone(c("tester", "accept passes; files outside its claim"))
        self.assertIsNone(c("engineer", "which ADR should I follow?"))

    def test_paths_and_marks(self):
        self.assertEqual(triage.paths_in("touch engine/a.cpp, ui/b.h and 1/2"),
                         ["engine/a.cpp", "ui/b.h"])
        n = triage.mark("x [triage:infra 1]", "infra", 2)
        self.assertEqual(triage.count(n, "infra"), 2)
        self.assertEqual(triage.count(n, "scope"), 0)


class Actions(support.DaemonCase):
    def setUp(self):
        super().setUp()
        self.o = self.make(dry_run=True, floors={"daw": 0})

    def task(self, tid, state="building", files=("engine/a.cpp",)):
        self.o.store.add_task(tid, "daw", "rt-engineer", state=state, files=files)
        return self.o.store.get_task(tid)

    def blocked(self, t, why, data=None):
        self.o._on_agent_blocked(t, "engineer", dict(data or {}, summary=why), 100.0)
        return self.o.store.get_task(t["id"])

    def test_infra_requeues_with_growing_backoff_attempts_kept(self):
        t = self.task("a")
        self.o.store.set_task("a", attempts=2)
        t1 = self.blocked(self.o.store.get_task("a"), "WinError 10061")
        self.assertEqual((t1["state"], t1["attempts"]), ("building", 2))
        self.assertEqual(t1["not_before"], 100.0 + triage.RETRY_S)
        t2 = self.blocked(t1, "WinError 10061")
        self.assertEqual(t2["not_before"], 100.0 + 2 * triage.RETRY_S)

    def test_nocommit_already_on_main_is_closed(self):
        self.task("b")
        self.forge.main_paths = ["engine/a.cpp"]
        t = self.blocked(self.o.store.get_task("b"), "No commits between main and feat/b")
        self.assertEqual(t["state"], "closed")

    def test_nocommit_not_on_main_requeues_once_then_blocks(self):
        self.task("c")
        t = self.blocked(self.o.store.get_task("c"), "No commits between main and feat/c")
        self.assertEqual(t["state"], "building")
        self.assertIn("commit", t["note"])
        t = self.blocked(t, "No commits between main and feat/c")
        self.assertEqual(t["state"], "blocked")

    def test_nocommit_with_commit_not_in_main_is_not_closed(self):
        self.task("d")
        self.forge.main_paths = ["engine/a.cpp"]
        t = self.blocked(self.o.store.get_task("d"), "No commits between main and feat/d",
                         {"commit": "abc"})
        self.assertEqual(t["state"], "building")

    def test_scope_widens_claim_and_goes_to_qa(self):
        self.task("e")
        t = self.blocked(self.o.store.get_task("e"),
                         "accept passes; blocked only on files outside its claim: ui/z.cpp")
        self.assertEqual(t["state"], "qa")
        self.assertEqual(t["files"], ["engine/a.cpp", "ui/z.cpp"])

    def test_scope_held_by_another_task_wakes_architect_and_requeues(self):
        self.task("f")
        self.task("g", files=("ui/z.cpp",))
        t = self.blocked(self.o.store.get_task("f"),
                         "accept passed, but files outside its claim: ui/z.cpp")
        self.assertEqual(t["state"], "building")
        self.assertEqual(t["files"], ["engine/a.cpp"])
        self.assertGreater(t["not_before"], 100.0)
        wakes = [x for x in self.o.store.tasks(kind="wake")
                 if x["agent"] == "architect"]
        self.assertEqual(len(wakes), 1)

    def test_other_blocks_still_block(self):
        self.task("h")
        t = self.blocked(self.o.store.get_task("h"), "tool missing")
        self.assertEqual(t["state"], "blocked")


if __name__ == "__main__":
    unittest.main()

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
        self.assertIsNone(c("engineer", "scope unclear: which files are mine?"))

    def test_scope_in_the_engineers_own_words(self):
        # eng-67's report, 2026-11-08: green per the tester, blocked on
        # scope only; it classified as nothing and the task sat blocked 3 h.
        real = ("Blocked on a scope decision (tester-113 bug). Commit 611aa555 "
                "is functionally green per tester-113 (stems, stems_sum, control, "
                "api_conformance_fake pass; null test 1.68e-08 < 1.19e-07). But 6 "
                "unclaimed files carry needed hunks: contracts/CONTRACTS.lock, "
                "engine/control/tests/test_control.cpp. No code changed this run.")
        c = triage.classify
        self.assertEqual(c("engineer", real), "scope")
        self.assertEqual(c("engineer", "tester-113 failed on scope only; "
                                       "ui/z.cpp is outside my claim"), "scope")
        self.assertEqual(c("engineer", "ctest control Passed; blocked: "
                                       "files outside the claim: ui/z.cpp"), "scope")
        self.assertIsNone(c("engineer", "tests fail; files outside my claim: ui/z.cpp"))

    def test_work_complete_blocked_on_globs_is_scope(self):
        # eng-207's report, 2026-11-09 (daw.model.session_mgmt.ui): "work is
        # complete" matched no green pattern, the task stayed blocked and the
        # feature's contract and model sat in integrating for 6 h.
        real = ("Work is complete at 6923c036 but the diff touches files "
                "outside the claimed globs. They are needed for the Save as "
                "Template category field and its tests: ui/qt/CMakeLists.txt, "
                "scenes/save_as_template.cpp, session_model_templates.cpp. "
                "Ask lead/architect to widen the globs.")
        self.assertEqual(triage.classify("engineer", real), "scope")
        self.assertIsNone(triage.classify(
            "engineer", "work is not complete; files outside my claim: a/b.c"))

    def test_paths_and_marks(self):
        self.assertEqual(triage.paths_in("touch engine/a.cpp, ui/b.h and 1/2"),
                         ["engine/a.cpp", "ui/b.h"])
        # A path closing a sentence keeps no punctuation.
        self.assertEqual(triage.paths_in("hunks: a/b.lock, c/d.cpp; and e/f.h."),
                         ["a/b.lock", "c/d.cpp", "e/f.h"])
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

    def test_scope_widens_to_the_branch_diff_not_the_reports_shorthand(self):
        # The report names "scenes/save_as_template.cpp" for
        # ui/qt/src/scenes/save_as_template.cpp and bare file names: widening
        # with the text gave globs that match nothing and QA bounced again.
        self.task("e2", files=("ui/qt/qml/Main.qml", "docs/images/x*.png"))
        self.forge.changed = {"feat/e2": [
            "ui/qt/qml/Main.qml", "docs/images/x-one.png",
            "ui/qt/src/scenes/save_as_template.cpp", "ui/qt/tests/rig.h"]}
        t = self.blocked(self.o.store.get_task("e2"),
                         "Work is complete but files outside the claimed globs: "
                         "scenes/save_as_template.cpp, rig.h")
        self.assertEqual(t["state"], "qa")
        self.assertEqual(t["files"], [
            "ui/qt/qml/Main.qml", "docs/images/x*.png",
            "ui/qt/src/scenes/save_as_template.cpp", "ui/qt/tests/rig.h"])

    def test_scope_with_nothing_outside_the_claim_goes_to_qa(self):
        self.task("e3")
        self.forge.changed = {"feat/e3": ["engine/a.cpp"]}
        t = self.blocked(self.o.store.get_task("e3"),
                         "accept passes; files outside its claim: x/y.cpp")
        self.assertEqual((t["state"], t["files"]), ("qa", ["engine/a.cpp"]))

    def test_scope_held_by_another_task_still_widens_and_names_the_holder(self):
        # The hunk is already made; claims serialise the next launch. Holding
        # the task for an architect decision left interchange.stems blocked
        # for 3 h on one shared test file (2026-11-08).
        self.task("f")
        self.o.store.set_task("f", feature_id="one")
        self.task("g", files=("ui/z.cpp",))
        self.o.store.set_task("g", feature_id="two")
        t = self.blocked(self.o.store.get_task("f"),
                         "accept passed, but files outside its claim: ui/z.cpp, ui/y.h.")
        self.assertEqual(t["state"], "qa")
        self.assertEqual(t["files"], ["engine/a.cpp", "ui/z.cpp", "ui/y.h"])
        ev = [e for e in self.o.store.events("f") if e["kind"] == "triage_scope"]
        self.assertEqual(len(ev), 1)
        self.assertIn("ui/z.cpp (g)", ev[0]["detail"])
        self.assertNotIn("ui/y.h (", ev[0]["detail"])
        self.assertEqual([x for x in self.o.store.tasks(kind="wake")
                          if x["agent"] == "architect"], [])

    def test_scope_without_a_path_wakes_architect_and_requeues(self):
        self.task("f2")
        t = self.blocked(self.o.store.get_task("f2"),
                         "accept passed, but blocked on files outside its claim")
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

    def test_blocked_on_a_question_the_mailbox_answers_is_requeued_once(self):
        # The lead's decision landed in eng-67's mailbox 20 s before the run
        # reported blocked on that very question (2026-11-08).
        self.task("i")
        self.o.store.set_task("i", cur_name="eng-67", eng_name="eng-67")
        self.o.broker.mail["eng-67"] = [
            {"type": "assign", "from": "daemon", "body": {"task": "i"}},
            {"type": "decision", "from": "lead",
             "body": "the 6 extra files are authorized; keep them"}]
        t = self.blocked(self.o.store.get_task("i"),
                         "no architect is live to decide; sent the question to the daemon")
        self.assertEqual(t["state"], "building")
        self.assertEqual(t["not_before"], 100.0)
        self.assertIn("decision from lead: the 6 extra files are authorized", t["note"])
        self.assertEqual(triage.count(t["note"], "decided"), 1)
        # Blocked again on the same answer: a real block, for a human.
        t = self.blocked(t, "still no decision")
        self.assertEqual(t["state"], "blocked")

    def test_no_decision_in_the_mailbox_blocks(self):
        self.task("j")
        self.o.store.set_task("j", cur_name="eng-1")
        self.o.broker.mail["eng-1"] = [{"type": "bug", "from": "tester-1", "body": "x"}]
        t = self.blocked(self.o.store.get_task("j"), "waiting on the architect")
        self.assertEqual(t["state"], "blocked")


if __name__ == "__main__":
    unittest.main()

"""A finished dependency's claim never refuses the task that continues it.

ui.mixer.sidechain, 2026-11-12: the ui slice passed QA holding
ui/qt/tests/test_insert_key.cpp (its claim was widened to its real diff),
the one file of ui-tests, which depends on it. The scheduler refused
ui-tests for the overlap, and the integrator that would have ended the ui
slice's claim waits for every sibling, ui-tests included: neither could
ever move, and the stall resolver was woken for it.
"""
import unittest

import support  # noqa: F401  (sets sys.path)
from test_daemon_unit import Base, plan

TEST_FILE = "ui/qt/tests/test_insert_key.cpp"


def engineers(p):
    return [ln.task_id for ln in p.launches if ln.kind == "engineer"]


class DependencyClaimTests(Base):
    def slice(self, tid, state="queued", files=(), deps=(), fid="f"):
        self.s.add_task(tid, "daw", "ui-engineer", state=state,
                        feature_id=fid, files=list(files),
                        depends_on=list(deps))

    def test_task_launches_over_its_finished_dependencys_claim(self):
        self.slice("f.ui", "integrating", ["ui/qt/src/key.cpp", TEST_FILE])
        self.slice("f.ui-tests", files=[TEST_FILE], deps=["f.ui"])
        self.assertEqual(engineers(plan(self.s)), ["f.ui-tests"])

    def test_claim_of_a_finished_dependency_further_up_the_chain(self):
        self.slice("f.contract", "integrating", [TEST_FILE])
        self.slice("f.ui", "integrating", ["ui/qt/src/key.cpp"],
                   deps=["f.contract"])
        self.slice("f.ui-tests", files=[TEST_FILE], deps=["f.ui"])
        self.assertEqual(engineers(plan(self.s)), ["f.ui-tests"])

    def test_finished_task_that_is_not_a_dependency_still_refuses(self):
        # Not built on that branch: both would edit the file apart.
        self.slice("f.ui", "integrating", [TEST_FILE])
        self.slice("f.other", files=[TEST_FILE])
        self.assertEqual(engineers(plan(self.s)), [])

    def test_another_live_claim_on_the_file_still_refuses(self):
        self.slice("f.ui", "integrating", [TEST_FILE])
        self.slice("g.x", "building", [TEST_FILE], fid="g")
        self.slice("f.ui-tests", files=[TEST_FILE], deps=["f.ui"])
        # g.x has no run here, so the plan relaunches it; only ours matters.
        self.assertNotIn("f.ui-tests", engineers(plan(self.s)))

    def test_dependency_still_in_work_keeps_the_task_waiting(self):
        self.slice("f.ui", "qa", [TEST_FILE])
        self.slice("f.ui-tests", files=[TEST_FILE], deps=["f.ui"])
        self.assertEqual(engineers(plan(self.s)), [])


if __name__ == "__main__":
    unittest.main()

"""A run's claims are released when the run ends, whatever its role."""
import os
import unittest

import support
from harness.brokerhost import BrokerHost


class ClaimReleaseTests(support.DaemonCase):
    def make_real(self):
        bdir = os.path.join(self.tmp, "bk")
        os.makedirs(bdir)
        host = BrokerHost(bdir)
        host.start()
        self.addCleanup(host.stop)
        return self.make(floors={"daw": 1}, broker=host), host

    def add_run(self, o, rid, tid):
        o.store.add_run(id=rid, task_id=tid, role="planner",
                        name="architect", pid=0, started=o.clock())

    def test_planner_claims_released_when_its_run_ends(self):
        o, host = self.make_real()
        o.store.add_task("p", "daw", "architect")
        self.add_run(o, "p-r1", "p")
        run = o.store.get_run("p-r1")
        self.assertEqual(host.claim_for("architect", ["brain/INDEX.md"]), [])
        denied = host.claim_for("eng-9", ["brain/**"])
        self.assertTrue(denied)  # held while the run lives
        o._end_run(run, "finished", 0)
        self.assertEqual(host.claim_for("eng-9", ["brain/**"]), [])

    def test_claims_kept_while_another_run_has_the_same_name(self):
        o, host = self.make_real()
        for rid in ("p-r1", "p-r2"):
            self.add_run(o, rid, "p")
        host.claim_for("architect", ["brain/INDEX.md"])
        o._end_run(o.store.get_run("p-r1"), "finished", 0)
        self.assertTrue(host.claim_for("eng-9", ["brain/**"]))

    def test_restart_sweeps_claims_of_dead_owners(self):
        o, host = self.make_real()
        host.claim_for("architect", ["brain/INDEX.md"])
        o.adopt_runs()
        self.assertEqual(host.claim_for("eng-9", ["brain/**"]), [])


if __name__ == "__main__":
    unittest.main()

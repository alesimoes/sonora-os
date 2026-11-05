"""The daemon with the real broker and a fake claude using the real client."""
import os
import unittest

import support
from harness.brokerhost import BrokerHost


def state(o, tid):
    return o.store.get_task(tid)["state"]


class IntegrationTests(support.DaemonCase):
    def test_agent_registers_sends_done_and_task_reaches_qa_then_tester(self):
        os.environ["FAKE_CLAUDE_DEFAULT"] = "client"
        host = BrokerHost(os.path.join(self.tmp, ".harness"))
        o = self.make(floors=dict(daw=1), broker=host)
        host.start()
        try:
            o.store.add_task("a", "daw", "rt-engineer", files=["a/**"],
                             feature_id="feat-a")
            self.assertTrue(self.pump(o, lambda: any(
                r["role"] == "tester" for r in o.store.runs_of("a"))))
            eng = o.store.runs_of("a")[0]
            self.assertEqual(eng["role"], "engineer")
            self.assertTrue(self.pump(o, lambda: state(o, "a")
                                      == "integrating"))
            msgs = [e["detail"] for e in o.store.events("a", "message")]
            self.assertIn("done from " + eng["name"], msgs)
            # per-feature channel and file claim were set up by the daemon
            self.assertIn(eng["name"], host.broker.channels["feat-a"])
            self.assertEqual(host.broker.claims.get("a/**"), eng["name"])
            self.assertEqual(len(host.token), 64)
        finally:
            host.stop()


if __name__ == "__main__":
    unittest.main()

"""Regression 2026-11-07: leads sent plan_request to 'orchestrator' and
'role:orchestrator'; the hub knew only 'daemon' (no_recipient), so no plan
was requested and the pools stayed under their floors, silently."""
import os
import tempfile
import types
import unittest

from tools.harness import protocol as P
from tools.harness.broker import Broker
from tools.harness.supervisor_mail import ALERT_TTL_S, check_undeliverable


class DaemonAliasTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.events = []
        self.b = Broker(os.path.join(self.tmp.name, ".harness"), port=0,
                        on_event=lambda k, **d: self.events.append((k, d)))

    def test_orchestrator_names_reach_the_daemon(self):
        for to in ("orchestrator", "role:orchestrator", "daemon"):
            _, rc = self.b.send("lead", to, "plan_request",
                                {"feature": "f", "track": "daw",
                                 "backlog_ids": ["x"]})
            self.assertEqual(rc, [P.DAEMON], to)
        got = [d["msg"]["to"] for k, d in self.events if k == P.EV_MESSAGE]
        self.assertEqual(len(got), 3)
        self.assertEqual([k for k, _ in self.events
                          if k == P.EV_UNDELIVERABLE], [])

    def test_undeliverable_plan_request_is_an_event(self):
        with self.assertRaises(P.ProtocolError) as cm:
            self.b.send("lead", "nobody", "plan_request", {"feature": "f"})
        self.assertEqual(cm.exception.code, P.E_NO_RECIPIENT)
        ev = [d for k, d in self.events if k == P.EV_UNDELIVERABLE]
        self.assertEqual((ev[0]["sender"], ev[0]["to"], ev[0]["type"]),
                         ("lead", "nobody", "plan_request"))

    def test_other_undeliverable_types_stay_plain_errors(self):
        with self.assertRaises(P.ProtocolError):
            self.b.send("eng-1", "nobody", "note")
        self.assertEqual(self.events, [])


class _SV:
    def __init__(self, rows):
        self.o = types.SimpleNamespace(undeliverable=rows)
        self.alerts = []

    def alert(self, id, text, level="alert", check=""):
        self.alerts.append((id, text, check))


class UndeliverableAlertTest(unittest.TestCase):
    def test_recent_failure_alerts(self):
        sv = _SV({("lead", "orchestrator", "plan_request"):
                  (100.0, "plan_request from lead to 'orchestrator' had no "
                          "recipient")})
        check_undeliverable(sv, 200.0)
        self.assertEqual(len(sv.alerts), 1)
        self.assertIn("no recipient", sv.alerts[0][1])
        self.assertEqual(sv.alerts[0][2], "mail")

    def test_old_failure_expires(self):
        sv = _SV({("lead", "x", "plan_request"): (0.0, "t")})
        check_undeliverable(sv, ALERT_TTL_S + 1)
        self.assertEqual(sv.alerts, [])
        self.assertEqual(sv.o.undeliverable, {})


if __name__ == "__main__":
    unittest.main()

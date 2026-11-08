"""Who an alert is for: the owner's queue holds only what needs the owner;
harness stalls fold into 'harness cuidando', grouped by their blocker."""
import io
import unittest
from contextlib import redirect_stdout

import support  # noqa: F401  (sets sys.path)
import console_read
import dashboard_console as V

NOW = 100000.0


def stall(i, text):
    return {"id": f"stall:{i}", "check": "stall", "level": "alert", "text": text, "since": NOW - 900}


def view(alerts, agents=()):
    o = {"updated_ts": NOW - 5, "cap": 8, "alerts": alerts}
    return V.build({"orchestrator": o, "board": {"agents": list(agents), "prs": []}}, True, NOW)


class OwnerQueue(unittest.TestCase):
    def test_ten_stalls_and_a_quiet_agent_leave_the_owner_queue_empty(self):
        alerts = [stall(i, f"t{i} sits in integrating with no agent: blocked sibling t0") for i in range(10)]
        quiet = {"name": "eng-9", "what": "x", "since": NOW - 3600, "last": NOW - 540}
        v = view(alerts, [quiet])
        self.assertEqual(v["attention"], [])
        self.assertEqual(v["strip"]["alerts"]["n"], 0)
        self.assertEqual(v["harness"]["n"], 10)

    def test_stalls_behind_one_blocker_are_one_line_naming_it_and_its_fixer(self):
        alerts = [stall(i, f"t{i} sits in queued: waits on feat-a") for i in range(4)]
        fixer = {"name": "eng-7", "what": "fix feat-a", "since": NOW - 60, "last": NOW - 5}
        g = view(alerts, [fixer])["harness"]["groups"]
        self.assertEqual(len(g), 1)
        self.assertEqual((g[0]["n"], g[0]["blocker"], g[0]["fixer"]), (4, "feat-a", "eng-7"))
        self.assertIn("feat-a", g[0]["title"])
        self.assertIn("eng-7", g[0]["reason"])

    def test_what_is_the_owners_stays_in_the_queue(self):
        alerts = [{"id": "daemon:down", "check": "daemon", "level": "alert", "text": "restart failed", "since": NOW},
                  {"id": "disk", "check": "disk", "level": "alert", "text": "2 GB", "since": NOW},
                  {"id": "daemon:restarted", "check": "daemon", "level": "warn", "text": "restarted", "since": NOW},
                  {"id": "x", "check": "queues", "level": "warn", "text": "q", "since": NOW}]
        v = view(alerts)
        self.assertEqual({i["key"] for i in v["attention"]}, {"alert:daemon:down", "alert:disk"})
        self.assertEqual(v["harness"]["n"], 2)

    def test_a_decision_for_the_owner_is_never_folded(self):
        o = {"updated_ts": NOW - 5, "waits_on_owner": [{"task": "t9", "title": "ADR"}], "alerts": [stall(1, "x")]}
        v = V.build({"orchestrator": o, "board": {}}, True, NOW)
        self.assertEqual([i["key"] for i in v["attention"]], ["owner:t9"])

    def test_no_signal_shows_only_after_the_restart_window(self):
        for quiet_s, shown in ((300, False), (1500, False), (2400, True)):
            a = {"name": "eng-1", "what": "x", "since": NOW - 9000, "last": NOW - quiet_s}
            self.assertEqual(bool(view([], [a])["attention"]), shown, quiet_s)


class Utf8(unittest.TestCase):
    def test_console_read_main_switches_the_streams_to_utf8(self):
        class Fake(io.StringIO):
            enc = None

            def reconfigure(self, encoding):
                self.enc = encoding
        out = Fake()
        import sys
        old = sys.stdout
        sys.stdout = out
        try:
            console_read.main(["nope"])
        finally:
            sys.stdout = old
        self.assertEqual(out.enc, "utf-8")


if __name__ == "__main__":
    unittest.main()

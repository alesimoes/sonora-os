"""harness/blockinfo.py + status.py: only a task its blocker marked `owner`
waits on the owner; every other blocked task is grouped by cause."""
import json
import os
import unittest

import support
from harness import blockinfo


def T(id, note="", **kw):
    return dict({"id": id, "state": "blocked", "branch": "b", "note": note,
                 "feature_id": None}, **kw)


class Classify(unittest.TestCase):
    def test_causes_from_notes_and_explicit_field(self):
        self.assertEqual(blockinfo.cause_of(T("a", "PR could not be opened: sh: WinError 2")), "infra")
        self.assertEqual(blockinfo.cause_of(T("a", "build container running")), "retry")
        self.assertEqual(blockinfo.cause_of(T("a", "files overlap with b")), "conflict")
        self.assertEqual(blockinfo.cause_of(T("a", "x", cause="conflict")), "conflict")

    def test_owner_only_when_marked(self):
        ts = [T("a", "WinError 2"), T("b", "pick one", owner="decision"),
              T("c", "janitor refused: container", cause="retry")]
        self.assertEqual([i["task"] for i in blockinfo.owner_items(ts, {})], ["b"])
        groups = blockinfo.blocked_groups(ts, {})
        self.assertEqual([(g["cause"], [i["task"] for i in g["items"]])
                          for g in groups], [("retry", ["c"]), ("infra", ["a"])])
        self.assertIn("tentar", groups[0]["label"])

    def test_superseded_task_drops_out(self):
        ts = [T("a", "x", feature_id="f", owner="d"), T("b", "y", feature_id="f")]
        feats = {"f": {"state": "merged"}}
        self.assertEqual(blockinfo.owner_items(ts, feats), [])
        self.assertEqual(blockinfo.blocked_groups(ts, feats), [])

    def test_only_a_janitor_busy_message_is_retry_later(self):
        self.assertTrue(blockinfo.is_retry_later("janitor", "refused: a build container is running"))
        self.assertFalse(blockinfo.is_retry_later("janitor", "disk full"))
        self.assertFalse(blockinfo.is_retry_later("engineer", "container running"))


class Daemon(support.DaemonCase):
    def status(self, o):
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            return json.load(f)

    def test_status_splits_owner_and_blocked(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        for tid in ("infra", "ask"):
            o.store.add_task(tid, "daw", "rt-engineer", state="blocked")
        o.store.set_task("infra", note="PR could not be opened: sh: WinError 2")
        o.store.set_task("ask", note="choose", owner="no default")
        st = self.status(o)
        self.assertEqual([w["task"] for w in st["waits_on_owner"]], ["ask"])
        self.assertEqual([i["task"] for g in st["blocked"] for i in g["items"]], ["infra"])

    def test_janitor_container_refusal_closes_the_wake_not_blocks(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.store.add_task("w", None, "janitor", kind="wake", state="building")
        t = o.store.get_task("w")
        o._on_agent_blocked(t, "janitor", {"summary": "build container running"}, 100.0)
        self.assertEqual(o.store.get_task("w")["state"], "closed")

    def test_agent_block_marks_owner_only_when_asked(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.store.add_task("a", "daw", "rt-engineer", state="building")
        o.store.add_task("b", "daw", "rt-engineer", state="building")
        o._on_agent_blocked(o.store.get_task("a"), "engineer", {"summary": "tool missing"}, 1.0)
        o._on_agent_blocked(o.store.get_task("b"), "engineer",
                            {"summary": "which ADR?", "needs_owner": "needs a choice"}, 1.0)
        a, b = o.store.get_task("a"), o.store.get_task("b")
        # The wording names no cause: "other", not the blanket "infra" that
        # read every block as an infrastructure fault (2026-11-13).
        self.assertEqual((a["owner"], a["cause"]), (None, "other"))
        self.assertEqual(b["owner"], "needs a choice")


if __name__ == "__main__":
    unittest.main()

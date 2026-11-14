"""A block whose note asks for the owner reaches waits_on_owner, and a
blocked task is classified by its wording, not stamped "infra"
(2026-11-13: waits_on_owner was [] and 28 of 28 blocks read infra)."""
import json
import os
import unittest

import support
from harness import blockinfo, owner_infer, triage_sweep

NOTES = {
    "aaf": "Nothing is broken in code; the stall is a missing owner "
           "decision. No feat/ branch exists yet.",
    "assistant": "claude.assistant waits on owner rulings (network, cost).",
    "pan": "Docker is down. Owner/host action: restart Docker engine.",
}


class Infer(unittest.TestCase):
    def test_asks_for_the_owner_in_so_many_words(self):
        self.assertEqual(owner_infer.owner_from(NOTES["aaf"]),
                         "Nothing is broken in code; the stall is a missing "
                         "owner decision.")
        for k in ("assistant", "pan"):
            self.assertIsNotNone(owner_infer.owner_from(NOTES[k]), k)

    def test_plain_mentions_do_not_count(self):
        for note in ("claim owner eng-164 must release the paths",
                     "the owner of the file is ui.mixer", "", None):
            self.assertIsNone(owner_infer.owner_from(note), note)


class Causes(unittest.TestCase):
    def cause(self, note, **kw):
        return blockinfo.cause_of(dict({"note": note}, **kw))

    def test_wording_beats_the_blanket_infra(self):
        self.assertEqual(self.cause("files of t1 overlap active feature x",
                                    cause="infra"), "conflict")
        self.assertEqual(self.cause("fails: forbidden edge a -> b",
                                    cause="infra"), "plan")
        self.assertEqual(self.cause("2 tests FAIL on the branch",
                                    cause="infra"), "code")
        self.assertEqual(self.cause("screenshot regeneration failed:\nx",
                                    cause="infra"), "infra")
        self.assertEqual(self.cause("590s cap killed it"), "infra")
        self.assertEqual(self.cause("odd", cause="infra"), "infra")
        self.assertEqual(self.cause("odd"), "other")
        self.assertEqual(self.cause("overlap", cause="retry"), "retry")


class Daemon(support.DaemonCase):
    def test_sweep_marks_the_ask_and_status_lists_it(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.store.add_task("w", None, "principal-engineer", kind="wake",
                         state="blocked")
        o.store.set_task("w", note=NOTES["aaf"], cause="infra")
        o.store.add_task("x", "daw", "rt-engineer", state="blocked")
        o.store.set_task("x", note="files of x overlap active feature y",
                         cause="infra")
        triage_sweep.sweep(o, o.clock())
        triage_sweep.sweep(o, o.clock())   # idempotent
        self.assertIn("missing owner decision", o.store.get_task("w")["owner"])
        self.assertEqual(len(o.store.events(kind="owner_inferred")), 1)
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            st = json.load(f)
        self.assertEqual([w["task"] for w in st["waits_on_owner"]], ["w"])
        self.assertEqual([g["cause"] for g in st["blocked"]], ["conflict"])

    def test_agent_block_asking_the_owner_is_an_owner_item(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.store.add_task("t", "daw", "rt-engineer", state="building")
        o._on_agent_blocked(o.store.get_task("t"), "engineer",
                            {"summary": NOTES["assistant"]}, o.clock())
        t = o.store.get_task("t")
        self.assertEqual((t["state"], t["cause"]), ("blocked", None))
        self.assertIn("waits on owner rulings", t["owner"])

    def test_agent_block_is_classified_not_stamped_infra(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.store.add_task("t", "daw", "rt-engineer", state="building")
        o._on_agent_blocked(o.store.get_task("t"), "engineer",
                            {"summary": "ctest -L ui FAILS: 2 cases red"},
                            o.clock())
        self.assertEqual(o.store.get_task("t")["cause"], "code")

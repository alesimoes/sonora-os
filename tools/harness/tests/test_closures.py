"""Closure-only results open no PR of their own (owner, 2026-11-14): a diff
that touches only brain/, planning/, docs/ or *.md is recorded and batched."""
import json
import os
import unittest

import support
from harness import closures

BR = "feat/f1"


class IsClosure(unittest.TestCase):
    def test_docs_only_is_a_closure(self):
        self.assertTrue(closures.closure_only(
            ["brain/decisions/ADR-1.md", "planning/x.json", "docs/a.png",
             "README.md"]))
        self.assertTrue(closures.closure_only([]))

    def test_any_code_or_unknown_diff_is_not(self):
        self.assertFalse(closures.closure_only(["brain/a.md", "engine/x.cpp"]))
        self.assertFalse(closures.closure_only(["tools/a.sh"]))
        self.assertFalse(closures.closure_only(None))


class Routing(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def ready(self, o, files, text="ADR text"):
        o.store.add_feature("f1", "daw", branch=BR)
        o.store.add_task("f1.a", "daw", "architect", state="integrating",
                         feature_id="f1", branch=BR)
        o.store.set_task("f1.a", integrated=1)
        o.store.set_meta("ready:f1", json.dumps(
            {"summary": "already on main", "commit": "abc123"}))
        self.forge.changed = {BR: files}
        wt = o.worktrees.ensure("f1.a", BR)
        os.makedirs(os.path.join(wt, "brain"), exist_ok=True)
        with open(os.path.join(wt, "brain", "ADR-9.md"), "w") as f:
            f.write(text)

    def test_code_diff_opens_a_pr_as_today(self):
        o = self.make()
        self.ready(o, ["engine/x.cpp"])
        o._open_pending_prs(o.clock())
        self.assertEqual(len(self.forge.created), 1)
        self.assertEqual(closures.pending(o.store), [])

    def test_docs_diff_is_recorded_and_opens_no_pr(self):
        o = self.make()
        self.ready(o, ["brain/ADR-9.md"])
        o._open_pending_prs(o.clock())
        self.assertEqual(self.forge.created, [])
        (e,) = closures.pending(o.store)
        self.assertEqual((e["id"], e["evidence"]), ("f1", "abc123"))
        self.assertEqual(e["files"], {"brain/ADR-9.md": "ADR text"})
        self.assertEqual(o.store.get_feature("f1")["state"], "closed")

    def test_batch_pr_carries_every_pending_closure_once(self):
        o = self.make()
        self.ready(o, ["brain/ADR-9.md"])
        o._open_pending_prs(o.clock())
        # the carried file is written into the batch worktree, which starts
        # without it (as a fresh checkout of main does)
        os.remove(os.path.join(o.worktrees.path("f1.a"), "brain", "ADR-9.md"))
        n = closures.open_batch(o, o.clock())
        self.assertEqual(len(self.forge.created), 1)
        c = self.forge.created[0]
        self.assertTrue(c["branch"].startswith(closures.BRANCH_PREFIX))
        self.assertIn("f1", c["title"])
        self.assertEqual(self.forge.enqueued,
                         [] if o.cfg.daemon_merge else [n])
        self.assertEqual(closures.pending(o.store), [])
        key = "closures-" + c["branch"][len(closures.BRANCH_PREFIX):]
        wt = o.worktrees.path(key)
        self.assertEqual(
            len(os.listdir(os.path.join(wt, "planning", "closures"))), 1)
        self.assertTrue(os.path.exists(os.path.join(wt, "brain", "ADR-9.md")))

    def test_batch_waits_for_its_interval_and_for_something_pending(self):
        o = self.make()
        self.assertIsNone(closures.open_batch(o, o.clock()))
        self.ready(o, ["brain/ADR-9.md"])
        o._open_pending_prs(o.clock())
        t0 = o.clock()
        self.assertIsNotNone(closures.open_batch(o, t0))
        self.forge.prs[100]["state"] = "merged"
        closures.record(o.store, o.store.get_feature("f1"), [], "/nope", [], t0)
        self.assertIsNone(closures.open_batch(o, t0 + 60))
        self.assertIsNotNone(
            closures.open_batch(o, t0 + closures.BATCH_EVERY_S + 1))

    def test_failed_batch_keeps_the_list(self):
        o = self.make()
        self.ready(o, ["brain/ADR-9.md"])
        o._open_pending_prs(o.clock())
        self.forge.fail.add("create_pr")
        self.assertIsNone(closures.open_batch(o, o.clock()))
        self.assertEqual(len(closures.pending(o.store)), 1)


if __name__ == "__main__":
    unittest.main()

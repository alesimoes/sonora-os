"""harness/triage_sweep.py: tasks blocked before the rules existed are
triaged on startup and hourly; a busy-janitor wake is closed."""
import unittest

import support
from harness import triage, triage_sweep


class Sweep(support.DaemonCase):
    def setUp(self):
        super().setUp()
        self.o = self.make(dry_run=True, floors={"daw": 0})

    def blocked(self, tid, note, files=("engine/a.cpp",), **kw):
        self.o.store.add_task(tid, "daw", "rt-engineer", state="building",
                              files=files)
        self.o.store.advance(tid, "blocked", note=note)
        if kw:
            self.o.store.set_task(tid, **kw)
        return tid

    def get(self, tid):
        return self.o.store.get_task(tid)

    def test_blocked_infra_task_is_requeued_with_backoff(self):
        self.blocked("a", "broker unreachable: WinError 10061")
        triage_sweep.sweep(self.o, 100.0)
        t = self.get("a")
        self.assertEqual(t["state"], "building")
        self.assertEqual(t["not_before"], 100.0 + triage.RETRY_S)

    def test_blocked_nocommit_already_on_main_is_closed(self):
        self.blocked("b", "No commits between main and feat/b")
        self.forge.main_paths = ["engine/a.cpp"]
        triage_sweep.sweep(self.o, 100.0)
        self.assertEqual(self.get("b")["state"], "closed")

    def test_blocked_nocommit_not_on_main_requeues_once(self):
        self.blocked("c", "No commits between main and feat/c")
        triage_sweep.sweep(self.o, 100.0)
        self.assertEqual(self.get("c")["state"], "building")

    def test_spent_nocommit_stays_blocked_and_quiet(self):
        self.blocked("d", "No commits between main and feat/d [triage:nocommit 1]")
        before = len(self.o.store.events("d"))
        self.assertEqual(triage_sweep.sweep(self.o, 100.0), 0)
        self.assertEqual(self.get("d")["state"], "blocked")
        self.assertEqual(len(self.o.store.events("d")), before)

    def test_owner_block_and_unclassified_block_are_left(self):
        self.blocked("e", "WinError 10061", owner="needs a choice")
        self.blocked("f", "tool missing")
        self.assertEqual(triage_sweep.sweep(self.o, 100.0), 0)
        self.assertEqual(self.get("e")["state"], "blocked")
        self.assertEqual(self.get("f")["state"], "blocked")

    def test_blocked_scope_task_is_widened_and_sent_to_qa(self):
        self.blocked("g", "accept passes; blocked only on files outside its claim: ui/z.cpp")
        triage_sweep.sweep(self.o, 100.0)
        self.assertEqual(self.get("g")["state"], "qa")

    def test_busy_janitor_wake_is_closed(self):
        self.o.store.add_task("w", None, "janitor", kind="wake", state="building")
        self.o.store.set_task("w", state="blocked",
                              note="refused: a build container is running")
        triage_sweep.sweep(self.o, 100.0)
        self.assertEqual(self.get("w")["state"], "closed")

    def test_tick_runs_at_startup_then_hourly(self):
        self.blocked("a", "WinError 10061")
        self.assertEqual(triage_sweep.tick(self.o, 100.0), 1)
        self.blocked("h", "WinError 10061")
        self.assertEqual(triage_sweep.tick(self.o, 200.0), 0)
        self.assertEqual(self.get("h")["state"], "blocked")
        self.assertEqual(
            triage_sweep.tick(self.o, 100.0 + triage_sweep.SWEEP_EVERY_S), 1)
        self.assertEqual(self.get("h")["state"], "building")


if __name__ == "__main__":
    unittest.main()

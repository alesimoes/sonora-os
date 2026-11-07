"""harness/nightly_supersede.py: a green nightly on main closes the queued or
building main-red tasks whose sha it contains, and stops their run."""
import unittest
from unittest import mock

import support
from harness import nightly, nightly_supersede

RED = "a" * 40
OTHER = "c" * 40
GREEN = "b" * 40


class Supersede(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def red_task(self, o, sha, state="queued"):
        tid = nightly.open_fix_task(o.store, sha, {"id": 1, "conclusion": "failure"}, "")
        if state != "queued":
            o.store.advance(tid, state)
        return tid

    def green(self, o, rid=9):
        self.forge.nightly_runs = [{
            "id": rid, "head_sha": GREEN, "status": "completed",
            "conclusion": "success", "url": f"https://x/runs/{rid}"}]
        o.tick()

    def test_queued_ancestor_is_closed_with_the_event(self):
        o = self.make()
        tid = self.red_task(o, RED)
        self.forge.ancestry = {(RED[:8], GREEN)}
        self.green(o)
        self.assertEqual(o.store.get_task(tid)["state"], "closed")
        ev = o.store.events(tid, "superseded_by_green")
        self.assertEqual([e["detail"] for e in ev], ["9"])

    def test_non_ancestor_task_is_left_alone(self):
        o = self.make()
        tid = self.red_task(o, OTHER)
        self.forge.ancestry = {(RED[:8], GREEN)}
        self.green(o)
        self.assertNotEqual(o.store.get_task(tid)["state"], "closed")

    def test_building_task_closes_and_its_active_run_is_stopped(self):
        o = self.make()
        tid = self.red_task(o, RED, state="building")
        o.store.add_run(id=f"{tid}-r1", task_id=tid, role="engineer",
                        name="eng-1", state="building", pid=None, started=1.0,
                        last_beat=1.0, result_path="r", stream_path="s",
                        worktree="w")
        self.forge.ancestry = {(RED[:8], GREEN)}
        with mock.patch.object(nightly_supersede.launcher, "kill_tree") as kt:
            closed = nightly_supersede.close_superseded(
                o, {"id": 9, "head_sha": GREEN})
        self.assertEqual(closed, [tid])
        kt.assert_called_once_with(None)
        run = o.store.get_run(f"{tid}-r1")
        self.assertEqual(run["outcome"], "superseded")
        self.assertIsNotNone(run["ended"])
        self.assertEqual(o.store.get_task(tid)["state"], "closed")

    def test_a_red_or_forge_error_closes_nothing(self):
        o = self.make()
        tid = self.red_task(o, RED)
        self.forge.ancestry = {(RED[:8], GREEN)}
        self.forge.fail.add("is_ancestor")
        self.green(o)
        self.assertNotEqual(o.store.get_task(tid)["state"], "closed")
        self.forge.fail.discard("is_ancestor")
        self.forge.nightly_runs = [{
            "id": 10, "head_sha": GREEN, "status": "completed",
            "conclusion": "failure", "url": "u"}]
        self.clock.offset += 1000
        o.tick()
        self.assertNotEqual(o.store.get_task(tid)["state"], "closed")


if __name__ == "__main__":
    unittest.main()

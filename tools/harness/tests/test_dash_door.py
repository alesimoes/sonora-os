"""The dashboard's door to the daemon (harness/dash_door.py): requests are
validated, applied on the daemon's own store on its tick, and logged as
events; a refused one changes nothing."""
import json
import os
import unittest

import support
from harness import cli_mailbox, dash_door

HEAD = "a" * 40


class Door(support.DaemonCase):
    def adopted(self, checks=None):
        o = self.make(floors={"daw": 0})
        self.forge.add_pr(10, "a", ["engine/x.cpp"], checks=checks or {})
        o.train.adopt()
        o.train._save("pr-10", head=HEAD, failing=[
            k for k, v in (checks or {}).items() if v == "fail"])
        return o

    def events(self, o, kind):
        return o.store.events(kind=kind)

    def test_validation_refuses_bad_params(self):
        for kind, args in (("regate", {}), ("regate", {"pr": "7"}),
                           ("regate", {"pr": True}), ("rerun-step", {"pr": 1}),
                           ("rerun-step", {"pr": 1, "step": "x" * 121}),
                           ("assign", {"pr": 1, "as": "boss"}),
                           ("nope", {"pr": 1})):
            with self.assertRaises(ValueError, msg=(kind, args)):
                dash_door.validate(kind, args)

    def test_post_then_take_oldest_first_and_files_removed(self):
        dash_door.post(self.tmp, "regate", pr=3)
        dash_door.post(self.tmp, "assign", pr=4, **{"as": "fixer"})
        got = cli_mailbox.take(self.tmp)
        self.assertEqual([k for k, _ in got], ["regate", "assign"])
        self.assertEqual(cli_mailbox.take(self.tmp), [])

    def test_regate_reruns_the_failed_jobs_on_github_and_logs(self):
        o = self.adopted({"Micro build / tsan": "fail", "Micro build / tree": "pass"})
        dash_door.post(o.hdir, "regate", pr=10)
        cli_mailbox.tick(o)
        self.assertIn((10, HEAD), self.forge.reruns)
        ev = self.events(o, "cli_request")
        self.assertEqual(len(ev), 1)
        self.assertIn("regate", ev[0]["detail"])

    def test_regate_is_refused_when_github_has_nothing_to_rerun(self):
        o = self.adopted({"build": "pass"})
        self.forge.rerun_failed = lambda pr, head: False
        dash_door.post(o.hdir, "regate", pr=10)
        cli_mailbox.tick(o)
        self.assertEqual(len(self.events(o, "cli_request_failed")), 1)
        self.assertEqual(self.events(o, "cli_request"), [])

    def test_rerun_step_reruns_only_that_job(self):
        o = self.adopted({"Micro build / tsan": "fail", "Micro build / asan": "fail"})
        dash_door.post(o.hdir, "rerun-step", pr=10, step="Micro build / tsan")
        cli_mailbox.tick(o)
        self.assertEqual(self.forge.rerun_jobs, [(10, "Micro build / tsan")])
        self.assertEqual(self.forge.prs[10]["checks"]["Micro build / asan"], "fail")
        self.assertEqual(len(self.events(o, "cli_request")), 1)

    def test_rerun_step_refused_for_a_job_that_did_not_fail(self):
        o = self.adopted({"tsan": "fail", "asan": "pass"})
        dash_door.post(o.hdir, "rerun-step", pr=10, step="asan")
        cli_mailbox.tick(o)
        self.assertEqual(len(self.events(o, "cli_request_failed")), 1)
        self.assertFalse(hasattr(self.forge, "rerun_jobs"))

    def test_unknown_pr_is_refused(self):
        o = self.adopted()
        dash_door.post(o.hdir, "regate", pr=99)
        cli_mailbox.tick(o)
        self.assertEqual(len(self.events(o, "cli_request_failed")), 1)

    def test_assign_sends_a_fixer_or_an_integrator(self):
        for who, kind in (("fixer", "ci"), ("integrator", "conflict")):
            o = self.adopted()
            dash_door.post(o.hdir, "assign", pr=10, **{"as": who})
            cli_mailbox.tick(o)
            t = o.store.get_task("pr-10")
            self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", kind))
            self.assertTrue(any(who in e["detail"]
                                for e in self.events(o, "cli_request")))

    def test_a_garbage_file_does_not_stop_the_tick(self):
        o = self.adopted()
        d = cli_mailbox.request_dir(o.hdir)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "1-1.json"), "w") as f:
            json.dump({"kind": "assign", "args": {"pr": 10}}, f)
        cli_mailbox.tick(o)
        self.assertEqual(len(self.events(o, "cli_request_failed")), 1)
        o.tick()   # the daemon's own tick runs the door too


if __name__ == "__main__":
    unittest.main()

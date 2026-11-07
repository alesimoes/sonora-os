"""A run's flakes and step files reach the store and the builder card; a
batch green after a rerun merges instead of being bisected; every run is
launched holding the machine lock for its whole length."""
import os
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import localci_flakes, localci_steps
from test_localci import Batching


class Flakes(unittest.TestCase):
    # Batching's fixture (git and bash replaced), not its tests
    setUp, finish, ready, state = (Batching.setUp, Batching.finish,
                                   Batching.ready, Batching.state)
    def finish_with(self, summary, flakes="", steps=()):
        (key, r), = self.ci._running()
        d = r["dir"]
        if flakes:
            with open(os.path.join(d, "flakes"), "w") as f:
                f.write(flakes)
        for name, line in steps:
            with open(os.path.join(d, name + ".status"), "w") as f:
                f.write(line)
        self.finish(0, summary)
        return key

    def test_every_run_holds_the_lock_for_its_whole_length(self):
        self.ready("a", "b")
        self.assertEqual(self.popen[0]["SONORA_LOCAL_CI_HOLD_LOCK"], "1")

    def test_green_after_a_rerun_merges_the_batch_and_records_the_flake(self):
        self.ready("a", "b")
        self.finish_with(
            "ok   tree (90 s, flaky: green on its rerun)\nlocal-ci: all passed\n",
            "step=tsan tests=session_saver,take_kill sha=abc\n")
        self.assertEqual((self.state("a"), self.state("b")), ("pass", "pass"))
        self.assertEqual(self.ci._queue("groups"), [])   # no bisect
        rows = localci_flakes.from_store(self.ci.o.store)
        self.assertEqual({(r["test"], r["step"]) for r in rows},
                         {("session_saver", "tsan"), ("take_kill", "tsan")})
        kinds = [e[1] for e in self.ci.o.store.events]
        self.assertEqual(kinds.count("flaky_test"), 2)
        self.assertEqual(self.ci.builder_status()["flakes"][0]["count"], 1)

    def test_backlog_lines_count_each_test(self):
        rows = [{"test": "control", "step": "tsan", "at": 1e9}] * 3 + \
            [{"test": "punch_points", "step": "tree", "at": 1e9}]
        lines = localci_flakes.backlog_lines(rows)
        self.assertEqual(len(lines), 2)
        self.assertIn("`control` in the tsan step, green on rerun 3x", lines[0])
        self.assertTrue(all(ln.startswith("- [ ] ") for ln in lines))

    def test_builder_card_names_the_running_step_from_its_status(self):
        self.ready("a")
        (key, r), = self.ci._running()
        for name, st in (("tree", "pass seconds=80 wait=12"),
                         ("tsan", "running seconds=0 wait=0")):
            with open(os.path.join(r["dir"], name + ".status"), "w") as f:
                f.write(f"state={st} sha=x\n")
        # a rerun step's first log is newer than every other log
        open(os.path.join(r["dir"], "tree.first.log"), "w").close()
        cur = self.ci.builder_status()["running"]
        self.assertEqual(cur["step"], "tsan")
        self.assertEqual(cur["steps"]["tree"]["wait"], 12)

    def test_a_new_run_in_the_same_directory_forgets_the_last(self):
        d = os.path.join(self.tmp, "logs")
        os.makedirs(d)
        for f in ("rc", "summary", "flakes", "tree.status", "tree.first.log",
                  "tree.log"):
            open(os.path.join(d, f), "w").close()
        localci_steps.clear_run(d)
        self.assertEqual(sorted(os.listdir(d)), ["tree.log"])

    def test_parse_status(self):
        self.assertEqual(localci_steps.parse_status(
            "state=pass seconds=12 wait=3 sha=ab\n"),
            {"state": "pass", "seconds": 12, "wait": 3, "sha": "ab"})
        self.assertIsNone(localci_steps.parse_status("state=odd seconds=1"))
        self.assertIsNone(localci_steps.parse_status("state=pass seconds=x"))
        self.assertIsNone(localci_steps.parse_status(""))


del Batching   # its tests run once, from test_localci

if __name__ == "__main__":
    unittest.main()

"""PR lifecycle (ADR-0047 4a step 6-7, section 5): integrator, one PR per
feature, CI watch, fixers, back-pressure, janitor, done message. gh and git
are the in-memory FakeForge; agents are the fake claude."""
import json
import os
import unittest

import support
from harness import launcher, scheduler

FAKE_ENV = "FAKE_CLAUDE_DEFAULT"


def state(o, tid):
    return o.store.get_task(tid)["state"]


def argv_log(o):
    path = os.path.join(os.environ["FAKE_CLAUDE_DIR"], "argv.log")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(x) for x in f if x.strip()]


def agents_launched(o):
    return [a[a.index("--agent") + 1] for a in argv_log(o)]


class Lifecycle(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})  # no lead wake for empty pools
        return super().make(**kw)

    def feature(self, o, fid, n=1, st="integrating", **kw):
        for i in range(n):
            o.store.add_task(f"{fid}{i}" if n > 1 else fid, "daw",
                             "rt-engineer", files=[f"{fid}/{i}/**"],
                             feature_id=fid, state=st, **kw)

    def open_feature(self, o, fid, pr, checks=None, mergeable="yes"):
        self.feature(o, fid, st="pr_open")
        o.store.set_feature(fid, pr=pr)
        self.forge.prs[pr] = {"branch": f"feat/{fid}", "files": [],
                              "state": "open", "mergeable": mergeable,
                              "checks": checks or {"build": "pass"}}

    def poll_again(self):
        self.clock.offset += 200  # past ci_poll_s

    # -- opening ---------------------------------------------------------
    def test_integrator_opens_one_pr_per_feature(self):
        o = self.make()
        self.feature(o, "f", n=2)
        self.assertTrue(self.pump(o, lambda: state(o, "f0") == "pr_open"))
        self.assertEqual(len(self.forge.created), 1)
        pr = self.forge.created[0]
        self.assertEqual((pr["base"], pr["branch"]), ("main", "feat/f"))
        self.assertEqual(self.forge.enqueued, [pr["number"]])
        self.assertEqual(self.forge.pushed, ["feat/f"])
        self.assertEqual(len(self.forge.pre_pr_calls), 1)
        self.assertIn("- f0 (rt-engineer)", pr["body"])
        self.assertIn("- a follow-up", pr["body"])
        self.assertIn("Generated with [Claude Code]", pr["body"])
        for t in ("f0", "f1"):
            self.assertEqual(state(o, t), "pr_open")
            self.assertEqual(o.store.get_task(t)["pr"], pr["number"])
        self.assertEqual(o.store.get_feature("f")["pr"], pr["number"])
        runs = [r for t in ("f0", "f1") for r in o.store.runs_of(t)]
        self.assertEqual([r["role"] for r in runs], ["integrator"])
        extra = o._extra(o.store.get_task("f0"), "integrator")
        self.assertIn("feat/f0, feat/f1", extra)  # merge order

    def test_lone_task_integrates_on_its_own_branch(self):
        o = self.make()
        self.feature(o, "solo")
        self.assertTrue(self.pump(o, lambda: state(o, "solo") == "pr_open"))
        self.assertEqual(self.forge.created[0]["branch"], "feat/solo")

    def test_integrator_waits_for_every_sibling(self):
        o = self.make()
        self.feature(o, "f", n=2)
        o.store.set_task("f1", state="qa")
        p = scheduler.plan(o.store.tasks(), [], o.cfg, self.clock())
        self.assertEqual([x for x in p.launches if x.kind == "integrator"], [])
        o.store.set_task("f1", state="integrating")
        p = scheduler.plan(o.store.tasks(), [], o.cfg, self.clock())
        self.assertEqual([(x.task_id, x.kind) for x in p.launches],
                         [("f0", "integrator")])

    def test_failed_pre_pr_sends_the_integrator_back(self):
        o = self.make()
        self.feature(o, "solo")
        self.forge.pre_pr_result = (False, "whitespace in a.cpp")
        self.assertTrue(self.pump(
            o, lambda: o.store.events("solo", "pre_pr_failed")))
        self.assertEqual(self.forge.created, [])
        t = o.store.get_task("solo")
        self.assertEqual((t["state"], t["integrated"]), ("integrating", 0))
        self.assertIn("whitespace in a.cpp", t["note"])
        self.assertEqual(t["attempts"], 1)

    def test_push_failure_retries_then_blocks(self):
        o = self.make()
        self.feature(o, "solo")
        self.forge.fail.add("push")
        self.assertTrue(self.pump(
            o, lambda: o.store.events("solo", "pr_open_failed")))
        self.assertEqual(state(o, "solo"), "integrating")
        for _ in range(4):
            self.clock.offset += 200
            o.tick()
        self.assertEqual(state(o, "solo"), "blocked")
        self.assertEqual(self.forge.created, [])

    def test_open_pr_adopted_not_duplicated(self):
        o = self.make()
        self.feature(o, "solo")
        self.forge.prs[7] = {"branch": "feat/solo", "files": [], "state": "open",
                             "mergeable": "yes", "checks": {}}
        self.assertTrue(self.pump(o, lambda: state(o, "solo") == "pr_open"))
        self.assertEqual(self.forge.created, [])
        self.assertEqual(o.store.get_feature("solo")["pr"], 7)

    # -- back-pressure ----------------------------------------------------
    def test_max_open_prs_holds_the_next_feature(self):
        o = self.make(max_open_prs=1)
        self.feature(o, "f1")
        self.feature(o, "f2")
        self.assertTrue(self.pump(o, lambda: len(self.forge.created) == 1))
        for _ in range(5):
            o.tick()
        self.assertEqual(len(self.forge.created), 1)
        waiting = "f2" if self.forge.created[0]["branch"] == "feat/f1" \
            else "f1"
        self.assertEqual(state(o, waiting), "integrating")
        self.assertEqual(o.store.runs_of(waiting), [])  # no integrator either
        self.forge.merge(self.forge.created[0]["number"])
        self.poll_again()
        self.assertTrue(self.pump(o, lambda: len(self.forge.created) == 2))

    def test_integrated_branch_waits_when_pr_slots_are_full(self):
        o = self.make(max_open_prs=1)
        self.open_feature(o, "busy", 50)
        self.feature(o, "ready")
        o.store.set_task("ready", integrated=1)
        o._open_pending_prs(self.clock())
        o._open_pending_prs(self.clock())
        self.assertEqual(self.forge.created, [])
        self.assertEqual(len(o.store.events("ready", "pr_deferred")), 1)
        self.assertEqual(state(o, "ready"), "integrating")

    def test_dry_run_never_writes(self):
        o = self.make(dry_run=True)
        self.feature(o, "solo")
        o.store.set_task("solo", integrated=1)
        o.tick()
        self.assertEqual((self.forge.created, self.forge.pushed,
                          self.forge.enqueued), ([], [], []))

    # -- CI watch ---------------------------------------------------------
    def test_red_check_gets_a_fixer_in_the_same_tick(self):
        o = self.make()
        self.open_feature(o, "p", 100, {"build": "fail", "lint": "pass"})
        o.tick()
        t = o.store.get_task("p")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "ci"))
        self.assertIn("red: build", t["note"])
        runs = o.store.runs_of("p")
        self.assertEqual([r["role"] for r in runs], ["fixer"])
        self.assertTrue(self.pump(o, lambda: state(o, "p") == "pr_open"))
        # the fixer is the track's engineer agent; the daemon pushes
        self.assertEqual(agents_launched(o), ["rt-engineer"])
        self.assertEqual(self.forge.pushed, ["feat/p"])
        self.assertEqual(self.forge.enqueued, [100])
        self.assertIsNone(o.store.get_task("p")["fix_kind"])

    def test_green_and_pending_checks_do_nothing(self):
        o = self.make()
        self.open_feature(o, "p", 100, {"build": "pending", "lint": "pass"})
        o.tick()
        self.assertEqual(state(o, "p"), "pr_open")
        self.assertEqual(o.store.runs_of("p"), [])

    def test_poll_is_rate_limited(self):
        o = self.make()
        self.open_feature(o, "p", 100)
        calls = []
        real = self.forge.pr_status
        self.forge.pr_status = lambda pr: calls.append(pr) or real(pr)
        for _ in range(4):
            o.tick()
        self.assertEqual(len(calls), 1)
        self.poll_again()
        o.tick()
        self.assertEqual(len(calls), 2)

    def test_same_job_red_after_two_fixes_blocks_the_feature(self):
        o = self.make()
        self.open_feature(o, "p", 100, {"build": "fail"})
        for _ in range(2):
            o.tick()
            self.assertEqual(state(o, "p"), "ci_fix")
            self.assertTrue(self.pump(o, lambda: state(o, "p") == "pr_open"))
            self.poll_again()
        self.assertEqual(o.store.get_feature("p")["fixes"], 2)
        o.tick()  # red a third time: the principal engineer, not a fixer
        self.assertEqual(state(o, "p"), "ci_fix")
        self.assertTrue(self.pump(o, lambda: state(o, "p") == "pr_open"))
        self.poll_again()
        o.tick()  # still red after the principal engineer: the owner
        self.assertEqual(state(o, "p"), "blocked")
        self.assertIn("build still red after 2 fixes and the principal",
                      o.store.get_task("p")["note"])
        self.assertEqual([r["role"] for r in o.store.runs_of("p")],
                         ["fixer", "fixer", "principal"])
        with open(os.path.join(o.odir, "waits-on-owner.md")) as f:
            self.assertIn("still red", f.read())

    def test_a_different_red_job_has_its_own_count(self):
        o = self.make()
        self.open_feature(o, "p", 100, {"build": "fail"})
        o.tick()
        self.assertTrue(self.pump(o, lambda: state(o, "p") == "pr_open"))
        self.poll_again()
        self.forge.checks(100, build="pass", asan="fail")
        o.tick()
        self.assertEqual(state(o, "p"), "ci_fix")

    def test_conflict_goes_to_the_integrator(self):
        o = self.make()
        self.open_feature(o, "p", 100, mergeable="no")
        o.tick()
        t = o.store.get_task("p")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "conflict"))
        run = o.store.runs_of("p")[0]
        self.assertEqual((run["role"], run["name"]), ("integrator",
                                                      "integrator"))
        self.assertTrue(self.pump(o, lambda: state(o, "p") == "pr_open"))
        self.assertEqual(agents_launched(o), ["integrator"])
        self.assertIn("Conflict run", argv_log(o)[0][-1])
        self.assertEqual(self.forge.pushed, ["feat/p"])
        self.assertEqual(self.forge.enqueued, [100])

    def test_conflict_runs_even_when_pr_slots_are_full(self):
        o = self.make(max_open_prs=1)
        self.open_feature(o, "p", 100, mergeable="no")
        o.tick()
        self.assertEqual([r["role"] for r in o.store.runs_of("p")],
                         ["integrator"])

    def test_poll_error_is_an_event_not_a_crash(self):
        o = self.make()
        self.open_feature(o, "p", 100)
        self.forge.fail.add("pr_status")
        o.tick()
        self.assertEqual(len(o.store.events("p", "ci_poll_failed")), 1)
        self.assertEqual(state(o, "p"), "pr_open")

    def test_closed_pr_blocks(self):
        o = self.make()
        self.open_feature(o, "p", 100)
        self.forge.prs[100]["state"] = "closed"
        o.tick()
        self.assertEqual(state(o, "p"), "blocked")

    # -- merged and janitor ------------------------------------------------
    def test_merge_and_janitor_every_three_merges(self):
        o = self.make()
        for i, fid in enumerate(("a", "b", "c")):
            self.open_feature(o, fid, 100 + i)
        for pr in (100, 101):
            self.forge.merge(pr)
        o.tick()
        self.assertEqual([state(o, x) for x in "ab"], ["merged", "merged"])
        self.assertEqual(o.store.tasks(kind="wake"), [])
        self.forge.merge(102)
        self.poll_again()
        o.tick()
        self.assertEqual(state(o, "c"), "merged")
        wakes = o.store.tasks(kind="wake")
        self.assertEqual(len(wakes), 1)
        self.assertIn("wake-janitor-janitor-after-merge-3", wakes[0]["id"])
        self.assertTrue(self.pump(
            o, lambda: o.store.tasks(kind="wake")[0]["state"] == "merged"))
        self.assertEqual(agents_launched(o), ["janitor"])

    # -- done message -------------------------------------------------------
    def test_done_message_advances_the_task_after_the_grace(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "hang")
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        self.assertTrue(self.pump(o, lambda: o.store.runs_of("a")))
        run = o.store.runs_of("a")[0]
        o.broker.emit("message", msg={"type": "done", "from": run["name"],
                                      "body": {"summary": "built it"}})
        o.tick()
        self.assertEqual(state(o, "a"), "building")  # result.json may follow
        self.clock.offset += o.cfg.done_grace_s + 1
        o.tick()
        self.assertEqual(state(o, "a"), "qa")
        self.assertEqual(o.store.get_run(run["id"])["outcome"], "finished")
        self.assertIn(("lead", "task_done", {"task": "a"}), o.broker.sent)

    def test_done_message_from_a_dead_process_counts_at_once(self):
        o = self.make(floors={"daw": 1})
        self.script("eng-1", "hang")
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        self.assertTrue(self.pump(o, lambda: o.store.runs_of("a")))
        run = o.store.runs_of("a")[0]
        o.broker.emit("message", msg={"type": "done", "from": run["name"],
                                      "body": "ok"})
        launcher.kill_tree(run["pid"])
        o.procs[run["id"]].wait(timeout=10)
        o.tick()
        self.assertEqual(state(o, "a"), "qa")

    def test_result_from_done_body(self):
        r = {"role": "tester", "task_id": "a"}
        d = support.orchestrator.Orchestrator._result_from_done(
            r, {"status": "fail", "summary": "no", "findings": [1]})
        self.assertEqual((d["status"], d["summary"], d["findings"]),
                         ("fail", "no", [1]))
        d = support.orchestrator.Orchestrator._result_from_done(r, "text")
        self.assertEqual((d["status"], d["summary"]), ("done", "text"))
        d = support.orchestrator.Orchestrator._result_from_done(
            r, {"status": "weird", "who": "x"})
        self.assertEqual(d["status"], "done")


if __name__ == "__main__":
    unittest.main()

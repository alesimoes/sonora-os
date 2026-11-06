"""Merge train (harness/train.py): adopted PRs move by the cheapest step,
the daemon merges green ones itself, a fixer only after a refresh stayed red.
gh and git are the in-memory FakeForge; agents are the fake claude."""
import json
import unittest
from unittest import mock

import support
from harness import launcher
from harness.forge import RateLimited
from harness.forge import job_signatures
from harness.train import agent_of, bundle_agent, track_of


def state(o, tid):
    return o.store.get_task(tid)["state"]


class Train(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        return super().make(**kw)

    def poll(self, o):
        self.clock.offset += 200   # past ci_poll_s
        o.tick()

    def info(self, o, n):
        return o.train.info(f"pr-{n}")

    # -- adoption ---------------------------------------------------------
    def test_adopt_makes_one_pr_open_feature_per_open_pr(self):
        o = self.make()
        self.forge.add_pr(10, "a", ["engine/x.cpp"])
        self.forge.add_pr(11, "b", ["iso/config/x"])
        self.assertEqual(o.train.adopt(), [10, 11])
        t = o.store.get_task("pr-11")
        self.assertEqual((t["state"], t["track"], t["agent"], t["branch"]),
                         ("pr_open", "os", "distro-engineer", "b"))
        self.assertEqual(o.store.get_feature("pr-10")["pr"], 10)
        self.assertEqual(o.train.adopt(), [])   # never twice
        o.tick()
        self.assertEqual(o.store.active_runs(), [])   # adoption launches nothing

    def test_adopt_only_the_numbers_given(self):
        o = self.make()
        self.forge.add_pr(10, "a")
        self.forge.add_pr(11, "b")
        self.assertEqual(o.train.adopt({11}), [11])

    def test_track_and_agent_from_files(self):
        self.assertEqual(track_of(["plugins/native/eq/a.cpp"]), "plugin")
        self.assertEqual(agent_of("plugin", ["plugins/native/eq/a.cpp"]),
                         "dsp-engineer")
        self.assertEqual(agent_of("plugin", ["plugins/host/a.cpp"]),
                         "plugin-engineer")
        self.assertEqual(agent_of("daw", ["ui/qt/a.cpp", "docs/x.md"]),
                         "ui-engineer")
        self.assertEqual(agent_of("daw", ["ui/qt/a.cpp", "engine/b.cpp"]),
                         "rt-engineer")

    def test_bundle_agent_is_the_one_most_files_belong_to(self):
        files = ["engine/session/a.cpp", "engine/session/b.cpp",
                 "common/session_files/c.h", "ui/qt/d.cpp",
                 "iso/config/package-lists/x", "tools/y.sh"]
        self.assertEqual(bundle_agent(files), ("daw", "rt-engineer"))
        self.assertEqual(bundle_agent(["iso/a", "iso/b", "engine/c.cpp"]),
                         ("os", "distro-engineer"))
        self.assertEqual(bundle_agent(["plugins/native/eq/a.cpp"]),
                         ("plugin", "dsp-engineer"))
        self.assertEqual(bundle_agent(["docs/a.md"]), ("daw", "rt-engineer"))

    # -- the same red twice ---------------------------------------------
    def red_feature(self, o):
        self.forge.add_pr(10, "a", ["engine/x.cpp"], checks={"Shell": "pass"})
        o.train.adopt()
        return o.store.get_feature("pr-10")

    def red(self, o, detail):
        f = o.store.get_feature("pr-10")
        o._red(f, o._feature_tasks("pr-10"), ["Shell"], "ci", detail)
        # the fixer is done; the PR is back for the next red
        for t in o._feature_tasks("pr-10"):
            if t["state"] == "ci_fix":
                o.store.advance(t["id"], "pr_open")
        o.store.set_feature("pr-10", state="pr_open")
        return state(o, "pr-10")

    def test_a_job_red_from_a_new_cause_is_not_the_same_red(self):
        o = self.make()
        self.red_feature(o)
        img = "[Shell]\n##[error]ghcr.io/x/labwc-test:trixie is missing"
        lock = ("[Shell]\nA contract file changed and CONTRACTS.lock was "
                "not regenerated.")
        self.assertEqual(self.red(o, img), "pr_open")
        self.assertEqual(self.red(o, img + " (run 123)"), "pr_open")
        # a third red with the image cause again would block; a new cause
        # starts the count over and gets a fixer
        f = o.store.get_feature("pr-10")
        o._red(f, o._feature_tasks("pr-10"), ["Shell"], "ci", lock)
        self.assertEqual(state(o, "pr-10"), "ci_fix")
        self.assertIn("CONTRACTS.lock", o.store.get_task("pr-10")["note"])

    def test_the_same_cause_three_times_escalates_then_blocks(self):
        o = self.make()
        self.red_feature(o)
        img = "[Shell]\n##[error]image abc1234def is missing (run 1)"
        self.red(o, img)
        self.red(o, img.replace("1)", "2)"))
        f = o.store.get_feature("pr-10")
        o._red(f, o._feature_tasks("pr-10"), ["Shell"], "ci",
               img.replace("abc1234def", "9876543fed"))
        self.assertEqual(state(o, "pr-10"), "ci_fix")
        self.assertTrue(o.store.get_task("pr-10")["escalated"])
        self.assertTrue(o.store.events("pr-10", "escalated"))
        for t in o._feature_tasks("pr-10"):
            o.store.advance(t["id"], "pr_open")
        o.store.set_feature("pr-10", state="pr_open")
        f = o.store.get_feature("pr-10")
        o._red(f, o._feature_tasks("pr-10"), ["Shell"], "ci", img)
        self.assertEqual(state(o, "pr-10"), "blocked")

    def test_job_signatures_split_the_excerpt_by_job(self):
        sig = job_signatures("[A]\nerror at line 12\n[B]\nboom")
        self.assertEqual(set(sig), {"A", "B"})
        self.assertEqual(sig["A"],
                         job_signatures("[A]\nerror at line 99")["A"])
        self.assertNotEqual(sig["A"], sig["B"])
        self.assertEqual(job_signatures(""), {})

    # -- green ------------------------------------------------------------
    def test_green_pr_is_merged_by_the_daemon_pinned_to_its_head(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pass", "doc": "pass"})
        o.train.adopt()
        o.tick()
        self.assertEqual(self.forge.merged, [(10, "h0")])
        self.assertEqual(state(o, "pr-10"), "merged")
        self.assertEqual(self.forge.enqueued, [])   # no detached watcher

    def test_pending_checks_wait(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pending"})
        o.train.adopt()
        o.tick()
        self.assertEqual(self.info(o, 10)["phase"], "CI rodando")
        self.assertFalse(getattr(self.forge, "merged", []))

    def test_merge_pool_dry_waits(self):
        o = self.make(live_pools=["fix"])
        self.forge.add_pr(10, "a", checks={"build": "pass"})
        o.train.adopt()
        o.tick()
        self.assertFalse(getattr(self.forge, "merged", []))
        self.assertIn("dry-run", self.info(o, 10)["next"])

    # -- red: refresh before any agent -----------------------------------
    def test_red_pr_is_refreshed_first_then_fixed(self):
        o = self.make()
        self.forge.add_pr(10, "a", ["engine/x.cpp"], checks={"build": "fail"})
        o.train.adopt()
        o.tick()
        # first red: main merged into the branch, no agent
        self.assertEqual(self.forge.updated, [10])
        self.assertEqual(o.store.active_runs(), [])
        self.assertEqual(self.info(o, 10)["phase"], "atualizado")
        # the new head is red too: now a fixer, on the PR's own branch
        self.forge.checks(10, build="fail")
        self.poll(o)
        t = o.store.get_task("pr-10")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "ci"))
        self.assertEqual([r["role"] for r in o.store.runs_of("pr-10")],
                         ["fixer"])
        self.assertEqual(o.worktrees.pr_checkouts, ["a"])
        self.assertTrue(self.pump(o, lambda: state(o, "pr-10") == "pr_open"))
        self.assertEqual(self.forge.pushed, ["a"])
        self.assertEqual(self.forge.enqueued, [])
        # the fixer's head is judged as is, never refreshed again
        self.forge.checks(10, build="pass")
        self.poll(o)
        self.assertEqual(self.forge.updated, [10])
        self.assertEqual(self.forge.merged, [(10, "h2")])

    def test_up_to_date_red_branch_reruns_its_failed_jobs(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "fail"})
        self.forge.prs[10]["update"] = "uptodate"
        o.train.adopt()
        o.tick()
        self.assertEqual(self.forge.reruns, [(10, "h0")])
        self.assertEqual(self.info(o, 10)["phase"], "rerodando")
        self.forge.checks(10, build="fail")
        self.poll(o)
        self.assertEqual(state(o, "pr-10"), "ci_fix")

    def test_cancelled_only_is_rerun_without_update(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "cancel", "doc": "pass"})
        o.train.adopt()
        o.tick()
        self.assertEqual(self.forge.reruns, [(10, "h0")])
        self.assertFalse(getattr(self.forge, "updated", []))

    def test_cancelled_three_times_on_one_head_blocks(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "cancel"})
        o.train.adopt()
        for _ in range(3):
            self.forge.checks(10, build="cancel")
            self.poll(o)
        self.assertEqual(state(o, "pr-10"), "blocked")
        self.assertEqual(len(self.forge.reruns), 2)

    def test_update_branch_conflict_goes_to_a_fixer(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "fail"})
        self.forge.prs[10]["update"] = "conflict"
        o.train.adopt()
        o.tick()
        t = o.store.get_task("pr-10")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "conflict"))

    def test_a_third_conflict_still_gets_an_integrator(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pass"}, mergeable="no")
        o.train.adopt()
        o.store.set_feature("pr-10", job_reds='{"merge-conflict": 2}')
        o.tick()
        t = o.store.get_task("pr-10")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "conflict"))

    def test_conflicting_pr_goes_to_a_fixer_with_the_train_brief(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pass"}, mergeable="no")
        o.train.adopt()
        o.tick()
        self.assertEqual(o.store.get_task("pr-10")["fix_kind"], "conflict")
        extra = o._extra(o.store.get_task("pr-10"), "fixer")
        self.assertIn("Merge-train PR #10", extra)
        self.assertIn("git merge origin/main", extra)

    def test_conflict_run_works_at_the_pr_head_and_is_pushed(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pass"}, mergeable="no")
        o.train.adopt()
        o.tick()
        runs = o.store.runs_of("pr-10")
        self.assertEqual([r["role"] for r in runs], ["integrator"])
        self.assertEqual(o.worktrees.pr_checkouts, ["a"])
        self.assertIn("Merge-train PR #10",
                      o._extra(o.store.get_task("pr-10"), "integrator"))
        self.forge.prs[10]["mergeable"] = "yes"
        self.assertTrue(self.pump(o, lambda: state(o, "pr-10") == "pr_open"))
        self.assertEqual(self.forge.pushed, ["a"])
        self.assertEqual(self.forge.enqueued, [])

    def test_parallel_conflict_runs_have_their_own_names(self):
        o = self.make(max_integrators=2)
        for n in (10, 11):
            self.forge.add_pr(n, f"b{n}", ["engine/shared.cpp"],
                              checks={"build": "pass"}, mergeable="no")
        o.train.adopt()
        o.tick()
        names = [r["name"] for r in o.store.active_runs()]
        self.assertEqual(len(names), 2)
        self.assertEqual(len(set(names)), 2)

    def test_fixer_that_commits_nothing_gets_a_rerun(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "fail"})
        self.forge.prs[10]["update"] = "uptodate"
        self.forge.fix_commits = False
        o.train.adopt()
        o.tick()                                 # rerun
        self.forge.checks(10, build="fail")
        with mock.patch.object(launcher, "git_head", return_value="h0"):
            self.poll(o)                         # fixer
            self.assertTrue(self.pump(
                o, lambda: state(o, "pr-10") == "pr_open"))
        self.assertEqual(len(self.forge.reruns), 2)
        self.assertEqual(self.info(o, 10)["phase"], "rerodando")

    def test_a_rerun_not_started_yet_sends_no_second_fixer(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "fail"})
        self.forge.prs[10]["update"] = "uptodate"
        self.forge.fix_commits = False
        o.train.adopt()
        o.tick()                                 # rerun
        self.forge.checks(10, build="fail")
        with mock.patch.object(launcher, "git_head", return_value="h0"):
            self.poll(o)                         # fixer, finds nothing
            self.assertTrue(self.pump(
                o, lambda: state(o, "pr-10") == "pr_open"))
        # GitHub still shows the old failure: the train waits for the rerun
        self.forge.checks(10, build="fail")
        self.clock.offset += 30
        o.ci_polled.clear()
        o.tick()
        self.assertEqual(state(o, "pr-10"), "pr_open")
        self.assertEqual(len(o.store.runs_of("pr-10")), 1)
        self.assertEqual(self.info(o, 10)["next"], "aguardando o rerun começar")

    # -- capacity, stacking, rate limits ---------------------------------
    def test_refreshes_wait_for_a_ci_slot(self):
        o = self.make(train_slots=2)
        for n in (10, 11, 12):
            self.forge.add_pr(n, f"b{n}", checks={"build": "cancel"})
        o.train.adopt()
        o.tick()
        self.assertEqual([p for p, _ in self.forge.reruns], [10, 11])
        self.assertEqual(self.info(o, 12)["phase"], "aguardando vaga")

    def test_red_pr_gets_a_fixer_when_no_slot_is_free(self):
        o = self.make(train_slots=1)
        self.forge.add_pr(10, "b10", ["x10/a.cpp"], checks={"build": "fail"})
        self.forge.add_pr(11, "b11", ["x11/a.cpp"], checks={"build": "fail"})
        o.train.adopt()
        o.tick()
        self.assertEqual(self.forge.updated, [10])
        t = o.store.get_task("pr-11")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "ci"))

    def test_stacked_pr_waits_then_is_retargeted(self):
        o = self.make()
        self.forge.add_pr(10, "base-br", checks={"build": "pending"})
        self.forge.add_pr(11, "top", base="base-br", checks={"build": "pass"})
        o.train.adopt()
        o.tick()
        i = self.info(o, 11)
        self.assertEqual((i["phase"], i["waits_on"]), ("empilhado", 10))
        self.forge.prs[10]["state"] = "merged"
        o.train._list = (0, [])                  # list cache expired
        self.poll(o)
        self.assertEqual(self.forge.prs[11]["base"], "main")
        self.poll(o)
        self.assertEqual(self.forge.merged, [(11, "h0")])

    def test_rate_limit_pauses_every_gh_call(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pass"})
        self.forge.add_pr(11, "b", checks={"build": "pass"})
        o.train.adopt()
        with mock.patch.object(self.forge, "pr_detail",
                               side_effect=RateLimited("HTTP 429")) as m:
            o.tick()
            self.assertEqual(m.call_count, 1)    # the second PR is not asked
            self.poll(o)
            self.assertEqual(m.call_count, 1)    # still paused
        self.assertGreater(o.train.pause_until, self.clock())
        self.clock.offset += 700
        o.tick()
        self.assertEqual(len(self.forge.merged), 2)

    # -- bundles -----------------------------------------------------------
    def test_bundle_joins_prs_of_one_context_into_one_pr(self):
        o = self.make()
        self.forge.add_pr(10, "eq", ["plugins/native/eq/a.cpp"],
                          checks={"build": "fail"}, title="EQ")
        self.forge.add_pr(11, "comp", ["plugins/native/comp/a.cpp"],
                          checks={"build": "pending"}, mergeable="no")
        self.forge.add_pr(12, "ui", ["ui/qt/a.cpp"], checks={"b": "pending"})
        o.train.adopt()
        o.train.bundle("plugins", [10, 11])
        self.forge.left_out = {"comp"}        # the integrator left #11 out
        o.tick()
        runs = o.store.runs_of("bundle-plugins")
        self.assertEqual([r["role"] for r in runs], ["integrator"])
        self.assertEqual(o.worktrees.starts, ["origin/main"])
        extra = o._extra(o.store.get_task("bundle-plugins"), "integrator")
        self.assertIn("Bundle run", extra)
        self.assertIn("#10 eq; #11 comp", extra)
        self.assertNotIn("Conflict run", extra)
        # members get no agent of their own while bundled
        self.assertEqual(o.store.runs_of("pr-10"), [])
        self.assertEqual(o.store.runs_of("pr-11"), [])
        self.assertTrue(self.pump(
            o, lambda: o.store.get_feature("bundle-plugins")["pr"]))
        n = o.store.get_feature("bundle-plugins")["pr"]
        created = self.forge.created[-1]
        self.assertEqual((created["branch"], created["base"]),
                         ("bundle/plugins", "main"))
        self.assertIn("- #10 EQ", created["body"])
        self.assertEqual([p for p, _ in self.forge.closed], [10])
        self.assertIn(f"#{n}", self.forge.closed[0][1])
        self.assertEqual(state(o, "pr-10"), "merged")
        self.assertIsNone(self.info(o, 11)["bundled_into"])   # back alone
        self.assertEqual(state(o, "bundle-plugins"), "pr_open")
        # the bundle PR then rides the train like any other
        self.forge.checks(n, build="pass")
        self.poll(o)
        self.assertIn(n, [p for p, _ in self.forge.merged])

    def test_red_bundle_gets_a_fixer_before_its_ci_ends(self):
        o = self.make()
        self.forge.add_pr(10, "a", ["engine/a.cpp"], checks={"b": "pass"})
        o.train.adopt()
        o.train.bundle("eng", [10])
        self.assertTrue(self.pump(
            o, lambda: o.store.get_feature("bundle-eng")["pr"]))
        n = o.store.get_feature("bundle-eng")["pr"]
        self.forge.checks(n, engine="fail", sanitizers="pending")
        self.poll(o)
        t = o.store.get_task("bundle-eng")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "ci"))
        self.assertFalse(getattr(self.forge, "updated", []))  # no refresh
        extra = o._extra(t, "fixer")
        self.assertIn(f"Merge-train PR #{n}", extra)

    def test_blocked_pr_merged_by_hand_leaves_the_board(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "cancel"})
        o.train.adopt()
        for _ in range(3):
            self.forge.checks(10, build="cancel")
            self.poll(o)
        self.assertEqual(state(o, "pr-10"), "blocked")
        self.forge.prs[10]["state"] = "merged"     # the owner merged it
        self.poll(o)
        self.assertEqual(state(o, "pr-10"), "merged")
        self.assertEqual(self.info(o, 10)["phase"], "mergeado")

    def test_bundle_refuses_a_pr_not_on_the_train(self):
        o = self.make()
        self.forge.add_pr(10, "a")
        o.train.adopt()
        with self.assertRaises(ValueError):
            o.train.bundle("x", [10, 99])

    def test_status_lists_the_train(self):
        o = self.make()
        self.forge.add_pr(10, "a", checks={"build": "pending", "doc": "pass"})
        o.train.adopt()
        o.tick()
        with open(o._p("status.json"), encoding="utf-8") as f:
            st = json.load(f)
        row = st["train"][0]
        self.assertEqual((row["pr"], row["phase"], row["passed"],
                          row["total"]), (10, "CI rodando", 1, 2))
        self.assertEqual(st["train_slots"], 6)



class FakeLocalCI:
    """harness/localci.py without git or bash: results set by the test."""
    def __init__(self):
        self.results, self.started, self.free = {}, [], True

    def result(self, sha):
        return self.results.get(sha)

    def prune(self, gone):
        pass

    def start(self, sha, full=False, pre=False):
        if not self.free:
            return False
        self.started.append(sha)
        self.results[sha] = {"state": "running"}
        return True

    def poll(self):
        pass

    def check_main(self):
        pass

    def builder_status(self, now=None):
        return {"running": None, "last": None,
                "queue": {"batch": [], "pre": [], "bisect": []}}


class LocalGate(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        o = super().make(local_ci=True, **kw)
        o.localci = FakeLocalCI()
        return o

    def green(self, o):
        self.forge.add_pr(10, "a", ["engine/x.cpp"], checks={"lint": "pass"})
        o.train.adopt()
        head = self.forge.pr_detail(10)["head_sha"]
        return head

    def test_green_pr_waits_for_the_local_run_then_merges(self):
        o = self.make()
        head = self.green(o)
        o.tick()
        self.assertEqual(o.localci.started, [head])
        self.assertEqual(getattr(self.forge, "merged", []), [])
        self.assertEqual(o.train.info("pr-10")["phase"], "CI local rodando")
        o.localci.results[head] = {"state": "pass"}
        self.clock.offset += 200
        o.tick()
        self.assertEqual(self.forge.merged, [(10, head)])

    def test_queued_pr_says_it_waits_for_the_batch(self):
        o = self.make()
        head = self.green(o)
        o.localci.results[head] = {"state": "running", "queued": True}
        o.tick()
        self.assertIn("lote", o.train.info("pr-10")["next"])
        self.assertEqual(getattr(self.forge, "merged", []), [])

    def test_no_free_slot_waits(self):
        o = self.make()
        self.green(o)
        o.localci.free = False
        o.tick()
        self.assertEqual(o.train.info("pr-10")["phase"], "aguardando CI local")
        self.assertEqual(getattr(self.forge, "merged", []), [])

    def test_local_failure_sends_a_fixer_with_the_log(self):
        o = self.make()
        head = self.green(o)
        o.localci.results[head] = {
            "state": "fail", "summary": "FAIL tree",
            "excerpt": "[tree]\nFAIL ui_window_layouts"}
        o.tick()
        t = o.store.get_task("pr-10")
        self.assertEqual((t["state"], t["fix_kind"]), ("ci_fix", "ci"))
        self.assertIn("ui_window_layouts", t["note"])
        self.assertEqual(getattr(self.forge, "merged", []), [])

    def test_conflict_with_main_goes_to_an_integrator(self):
        o = self.make()
        head = self.green(o)
        o.localci.results[head] = {"state": "fail",
                                   "summary": "conflicts with main"}
        o.tick()
        self.assertEqual(o.store.get_task("pr-10")["fix_kind"], "conflict")

    def test_heavy_github_jobs_are_ignored_when_the_gate_is_on(self):
        o = self.make()
        self.forge.add_pr(10, "a", ["engine/x.cpp"], checks={
            "lint": "pass", "Qt interface build and headless tests": "fail"})
        o.train.adopt()
        head = self.forge.pr_detail(10)["head_sha"]
        o.localci.results[head] = {"state": "pass"}
        o.tick()
        self.assertEqual(self.forge.merged, [(10, head)])

    def test_gate_off_merges_on_github_checks_alone(self):
        o = super().make(floors={"daw": 0})
        self.green(o)
        o.tick()
        self.assertEqual(len(self.forge.merged), 1)

if __name__ == "__main__":
    unittest.main()

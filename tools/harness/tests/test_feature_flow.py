"""Feature flow end to end (ADR-0047 4a): a fake architect and a fake lead
drive a 3-task feature through planning, distributing, building, qa and
integrating to ONE PR on the FakeForge, then merged. Real fake-claude child
processes; no quota."""
import json
import os
import unittest

import support
from harness import launcher

FEATURE = "time.varispeed"


def canned_plan(feature=FEATURE):
    def task(i, kind, agent, files, deps):
        return {"id": i, "kind": kind, "title": f"{kind} work",
                "brief": f"do the {kind} part", "files": files,
                "depends_on": deps, "agent": agent, "accept": [f"test {i}"]}
    return {
        "version": 1, "feature": feature, "backlog_ids": ["time.varispeed"],
        "track": "daw", "feature_branch": f"feat/{feature}",
        "reserved": {"adr": "auto", "protocol": None, "formats": []},
        "adr": "auto",
        "tasks": [task("t1", "contract", "rt-engineer", ["contracts/vs/**"],
                       []),
                  task("t2", "engine", "rt-engineer", ["engine/vs/**"],
                       ["t1"]),
                  task("t3", "ui", "ui-engineer", ["ui/vs/**"], ["t1"])],
        "merge_order": ["t1", "t2", "t3"],
        "pr": {"title": "varispeed", "partial": False,
               "partial_reason": None}}


def state(o, tid):
    return o.store.get_task(tid)["state"]


def fstate(o, fid=FEATURE):
    return o.store.get_feature(fid)["state"]


def prompts(name):
    path = os.path.join(os.environ["FAKE_CLAUDE_DIR"], "argv.log")
    out = []
    with open(path) as f:
        for line in f:
            a = json.loads(line)
            if a[a.index("--agent") + 1] == name:
                out.append(a[-1])
    return out


class FlowCase(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        o = super().make(**kw)
        self.plan(FEATURE)
        return o

    def plan(self, feature, plan=None):
        with open(os.path.join(self.fake_dir, f"plan-{feature}.json"),
                  "w") as f:
            json.dump(plan or canned_plan(feature), f)

    def open(self, o, fid=FEATURE, track="daw", wants=("adr",)):
        o.flow.open_feature(fid, track, [fid], brief="vari-speed",
                            wants=wants)


class HappyPath(FlowCase):
    def test_three_task_feature_to_one_pr(self):
        o = self.make()
        self.script("architect", "architect")
        self.script("lead", "lead")
        self.open(o)
        self.assertEqual(fstate(o), "queued")
        # a message already in the feature channel is replayed to the run
        log = os.path.join(o.hdir, "log")
        os.makedirs(log)
        with open(os.path.join(log, "messages.jsonl"), "w") as f:
            f.write(json.dumps({"ts": "T0", "from": "owner", "to": "lead",
                                "type": "note", "feature": FEATURE,
                                "body": {"text": "prefer 2x cap"}}) + "\n")
            f.write(json.dumps({"ts": "T1", "from": "x", "to": "y",
                                "type": "note", "feature": "other",
                                "body": {"text": "not mine"}}) + "\n")

        self.assertTrue(self.pump(o, lambda: fstate(o) == "pr_open",
                                  timeout=60))
        self.assertEqual(len(self.forge.created), 1)
        pr = self.forge.created[0]
        self.assertEqual((pr["base"], pr["branch"]),
                         ("main", f"feat/{FEATURE}"))
        self.assertEqual(self.forge.enqueued, [pr["number"]])
        self.assertEqual(len(self.forge.pre_pr_calls), 1)
        f = o.store.get_feature(FEATURE)
        self.assertEqual(f["pr"], pr["number"])
        self.assertEqual(f["adr_no"], 11)   # seed 10 + 1, reserved by us
        self.assertTrue(os.path.exists(f["plan_path"]))

        ts = {t["id"]: t for t in o.store.feature_tasks(FEATURE)}
        self.assertEqual(sorted(ts), [f"{FEATURE}.t1", f"{FEATURE}.t2",
                                      f"{FEATURE}.t3"])
        for t in ts.values():
            self.assertEqual((t["state"], t["pr"]), ("pr_open", pr["number"]))
            self.assertEqual(t["adr_no"], 11)
        t2 = ts[f"{FEATURE}.t2"]
        self.assertEqual(t2["branch"], f"sub/{FEATURE}/t2")
        self.assertEqual(t2["depends_on"], [f"{FEATURE}.t1"])
        self.assertEqual(ts[f"{FEATURE}.t3"]["agent"], "ui-engineer")
        # dependants start from the contract's sub-branch
        bases = o.worktrees.bases
        self.assertEqual(bases[f"{FEATURE}.t1"], f"feat/{FEATURE}")
        self.assertEqual(bases[f"{FEATURE}.t2"], f"sub/{FEATURE}/t1")
        # the integrator merges in the plan's merge_order
        extra = o._extra(ts[f"{FEATURE}.t1"], "integrator")
        self.assertIn(f"sub/{FEATURE}/t1, sub/{FEATURE}/t2, "
                      f"sub/{FEATURE}/t3", extra)
        # one run per engineer, one tester per task, one integrator
        roles = [r["role"] for t in ts.values()
                 for r in o.store.runs_of(t["id"])]
        self.assertEqual(sorted(roles), ["engineer"] * 3 + ["integrator"]
                         + ["tester"] * 3)
        # event-driven wakes: architect once, lead for plan_ready and each
        # task_done; each started with the channel log in its prompt
        self.assertEqual(len(prompts("architect")), 1)
        lead = prompts("lead")
        self.assertIn("event: plan_ready", lead[0])
        self.assertIn("prefer 2x cap", lead[0])
        self.assertNotIn("not mine", lead[0])
        self.assertIn("Channel log of this feature", prompts("architect")[0])
        self.assertTrue(self.pump(o, lambda: sum(
            "event: task_done" in p for p in prompts("lead")) == 3))
        for p in (*lead, *prompts("architect")):
            self.assertEqual(p.count("reserved by the daemon"), 1)
        # the assignment table was completed with the engineer names
        with open(o.flow.assign_file(FEATURE)) as fh:
            table = json.load(fh)
        self.assertTrue(all(r["engineer_name"].startswith("eng-")
                            for r in table["rows"]))
        tells = [(to, ty) for to, ty, _ in o.broker.sent]
        self.assertEqual(sum(1 for _, ty in tells if ty == "assign"), 3)

        self.forge.merge(pr["number"])
        self.clock.offset += 200
        self.assertTrue(self.pump(o, lambda: fstate(o) == "merged"))
        self.assertEqual(len(self.forge.created), 1)

    def test_feature_state_follows_the_tasks(self):
        o = self.make()
        self.script("architect", "architect")
        self.script("lead", "lead")
        os.environ["FAKE_CLAUDE_DEFAULT"] = "sleep:0.05"
        self.open(o)
        seen = []

        def done():
            s = fstate(o)
            if not seen or seen[-1] != s:
                seen.append(s)
            return s == "pr_open"
        self.assertTrue(self.pump(o, done, timeout=60))
        order = ["queued", "planning", "distributing", "building", "qa",
                 "integrating", "pr_open"]
        idx = [order.index(s) for s in seen]
        self.assertEqual(idx, sorted(idx), seen)
        for must in ("planning", "distributing", "building"):
            self.assertIn(must, seen)

    def test_dependants_wait_for_the_contract_task(self):
        """t2 and t3 wait for the contract task t1 to pass qa."""
        o = self.make()
        self.script("architect", "architect")
        self.script("lead", "lead")
        self.open(o)
        self.assertTrue(self.pump(o, lambda: o.store.get_task(
            f"{FEATURE}.t1") is not None, timeout=30))
        first = {r["task_id"] for t in o.store.feature_tasks(FEATURE)
                 for r in o.store.runs_of(t["id"]) if r["role"] == "engineer"}
        self.assertLessEqual(first, {f"{FEATURE}.t1"})


class InvalidFiles(FlowCase):
    def test_invalid_plan_is_rejected_and_the_architect_relaunched(self):
        o = self.make()
        self.script("architect", "architect-bad", "architect")
        self.script("lead", "lead")
        self.open(o)
        self.assertTrue(self.pump(o, lambda: fstate(o) == "building",
                                  timeout=30))
        self.assertEqual(len(o.store.events(kind="plan_invalid")), 1)
        pr = prompts("architect")
        self.assertEqual(len(pr), 2)
        self.assertIn("rejected by the daemon", pr[1])
        self.assertIn("overlap", pr[1])
        self.assertNotIn("rejected", pr[0])
        # the rejected file created no task
        self.assertEqual(len(o.store.feature_tasks(FEATURE)), 3)

    def test_plan_invalid_forever_blocks_the_feature(self):
        o = self.make()
        self.script("architect", "architect-bad")
        self.open(o)
        self.assertTrue(self.pump(o, lambda: fstate(o) == "blocked",
                                  timeout=30))
        self.assertEqual(o.store.feature_tasks(FEATURE), [])
        self.assertEqual(len(prompts("architect")), 4)  # 1 + 3 relaunches
        with open(os.path.join(o.odir, "waits-on-owner.md")) as f:
            self.assertIn(FEATURE, f.read())

    def test_invalid_assignments_are_rejected_and_the_lead_relaunched(self):
        o = self.make()
        self.script("architect", "architect")
        self.script("lead", "lead-bad", "lead")
        self.open(o)
        self.assertTrue(self.pump(o, lambda: fstate(o) == "building",
                                  timeout=30))
        self.assertEqual(len(o.store.events(kind="assignments_invalid")), 1)
        leads = [p for p in prompts("lead") if "event: plan_ready" in p]
        self.assertEqual(len(leads), 2)
        self.assertIn("no row for task", leads[1])

    def test_plan_overlapping_an_active_feature_is_rejected(self):
        o = self.make()
        self.script("architect", "architect")
        o.store.add_task("other", "daw", "rt-engineer",
                         files=["engine/vs/**"], state="building")
        self.open(o)
        self.assertTrue(self.pump(
            o, lambda: o.store.events(kind="plan_invalid"), timeout=30))
        ev = o.store.events(kind="plan_invalid")[0]["detail"]
        self.assertIn("overlap active feature other", ev)

    def test_missing_plan_file_is_a_rejection_too(self):
        o = self.make()
        os.remove(os.path.join(self.fake_dir, f"plan-{FEATURE}.json"))
        self.script("architect", "done")   # exits without writing a plan
        self.open(o)
        self.assertTrue(self.pump(
            o, lambda: o.store.events(kind="run_failed"), timeout=30))
        self.assertIn("invalid file",
                      o.store.events(kind="run_failed")[0]["detail"])


class Promotion(FlowCase):
    def test_numbers_are_reserved_when_planning_opens(self):
        o = self.make(max_active_features=1)
        self.script("architect", "sleep:0.2")
        self.open(o, "a")
        self.open(o, "b")
        o.tick()
        self.assertEqual(fstate(o, "a"), "planning")
        self.assertEqual(fstate(o, "b"), "queued")   # one at a time
        self.assertEqual(o.store.get_feature("a")["adr_no"], 11)
        self.assertIsNone(o.store.get_feature("b")["adr_no"])
        res = [r for r in o.store.reservations() if r["kind"] == "adr"]
        self.assertEqual([r["task_id"] for r in res], ["feature:a"])

    def test_two_features_never_share_a_number(self):
        o = self.make()
        self.script("architect", "sleep:0.2")
        self.open(o, "a", wants=("adr", "protocol"))
        self.open(o, "b", wants=("adr", "protocol"))
        o.tick()
        a, b = (o.store.get_feature(x) for x in "ab")
        self.assertNotEqual(a["adr_no"], b["adr_no"])
        self.assertNotEqual(a["protocol_no"], b["protocol_no"])

    def test_a_feature_waits_for_the_feature_it_names(self):
        o = self.make()
        o.flow.open_feature("base", "daw", ["x.base"])
        o.flow.open_feature("top", "daw", ["x.top"],
                            waits_on="x.base merged")
        self.script("architect", "sleep:0.2")
        o.tick()
        self.assertEqual(fstate(o, "base"), "planning")
        self.assertEqual(fstate(o, "top"), "queued")

    def test_dry_run_plans_nothing(self):
        o = self.make(dry_run=True)
        self.open(o)
        for _ in range(3):
            o.tick()
        self.assertEqual(fstate(o), "queued")
        self.assertEqual(len(o.store.events(kind="dry_run_promote")), 1)
        self.assertEqual(o.store.tasks(kind="wake"), [])

    def test_lead_proposal_opens_a_queued_feature(self):
        o = self.make()
        o.store.add_run(id="r1", task_id="x", role="planner", name="lead",
                        started=self.clock(), last_beat=self.clock())
        body = {"feature": "ui.mixer.io", "track": "daw",
                "backlog_ids": ["ui.mixer.io"]}
        o.on_broker_event("message", msg={"from": "lead",
                                          "type": "plan_request",
                                          "body": body})
        self.assertEqual(fstate(o, "ui.mixer.io"), "queued")
        bad = dict(body, feature="midi.import", backlog_ids=["midi.import"])
        o.on_broker_event("message", msg={"from": "lead",
                                          "type": "plan_request",
                                          "body": bad})
        self.assertIsNone(o.store.get_feature("midi.import"))
        self.assertEqual(len(o.store.events(kind="proposal_rejected")), 1)


class Adoption(FlowCase):
    """The daemon dies and starts again: its claude children keep running."""

    def drop(self, o):
        """The daemon dies: no _kill_all, the children outlive it."""
        o.close()
        self.orchs.remove(o)

    def restart(self, o):
        self.drop(o)
        return self.make_again()

    def make_again(self):
        import orchestrator
        from harness import scheduler
        c = scheduler.Config(cap=20, floors={"daw": 0}, backoff_s=(0, 0, 0),
                             local_builds=3)
        o2 = orchestrator.Orchestrator(
            self.tmp, cfg=c, broker=support.StubBroker(), dry_run=False,
            fake_claude=support.FAKE, clock=self.clock, forge=self.forge,
            worktrees=support.DirWorktrees(self.tmp),
            seed_fn=lambda k: 10)
        self.orchs.append(o2)
        return o2

    def test_running_child_is_adopted_not_relaunched(self):
        o = self.make()
        os.environ["FAKE_CLAUDE_DEFAULT"] = "sleep:1.5"
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        o.tick()
        (run,) = o.store.active_runs()
        self.assertEqual(run["role"], "engineer")
        self.drop(o)
        o2 = self.make_again()
        self.assertEqual(o2.adopt_runs(), [run["id"]])
        self.assertEqual(o2.broker.expected, [run["name"]])
        self.assertEqual(len(o2.store.events("a", "adopted")), 1)
        os.environ["FAKE_CLAUDE_DEFAULT"] = "done"
        self.assertTrue(self.pump(o2, lambda: state(o2, "a") == "qa",
                                  timeout=30))
        engineers = [r for r in o2.store.runs_of("a")
                     if r["role"] == "engineer"]
        self.assertEqual(len(engineers), 1)   # never launched twice
        self.assertEqual(engineers[0]["outcome"], "finished")
        self.assertEqual(engineers[0]["id"], run["id"])

    def test_child_that_died_while_the_daemon_was_down_is_relaunched(self):
        o = self.make()
        os.environ["FAKE_CLAUDE_DEFAULT"] = "hang"
        o.store.add_task("a", "daw", "rt-engineer", files=["a/**"])
        o.tick()
        (run,) = o.store.active_runs()
        launcher.kill_tree(run["pid"])
        o.procs[run["id"]].wait(timeout=10)
        self.drop(o)
        o2 = self.make_again()
        self.assertEqual(o2.adopt_runs(), [])
        os.environ["FAKE_CLAUDE_DEFAULT"] = "done"
        self.assertTrue(self.pump(o2, lambda: state(o2, "a") == "qa",
                                  timeout=30))
        runs = [r for r in o2.store.runs_of("a") if r["role"] == "engineer"]
        self.assertEqual([r["outcome"] for r in runs], ["dead", "finished"])
        self.assertEqual(runs[0]["name"], runs[1]["name"])  # same engineer

    def test_flow_resumes_after_restart_in_the_middle_of_planning(self):
        o = self.make()
        self.script("architect", "sleep:1.5:architect")
        self.script("lead", "lead")
        self.open(o)
        o.tick()
        self.assertEqual(fstate(o), "planning")
        o2 = self.restart(o)
        o2.adopt_runs()
        self.assertTrue(self.pump(o2, lambda: fstate(o2) == "pr_open",
                                  timeout=60))
        self.assertEqual(len(prompts("architect")), 1)  # adopted, not rerun
        self.assertEqual(len(self.forge.created), 1)


if __name__ == "__main__":
    unittest.main()

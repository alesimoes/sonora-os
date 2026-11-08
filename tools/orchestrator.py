#!/usr/bin/env python3
"""SonoraOS harness orchestrator daemon (ADR-0047). Standard library only.

    orchestrator.py run [--live] [--once] [--fake-claude PATH]
    orchestrator.py status | drain | stop | resume | flakes
    orchestrator.py pause --pool NAME [--resume]
    orchestrator.py add --id ID --track daw --agent rt-engineer ...
    orchestrator.py adopt [PR...] | bundle NAME PR... | import-queue PATH

The command line is harness/cli.py: a command that changes the store goes
through the running daemon (harness/daemonlock.py, harness/cli_mailbox.py).

The daemon decides nothing that needs judgement. Roles decide; this file
enforces pools, caps, retries, kill switches and number reservations.
"""
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (autoadopt, closed, cli, cli_mailbox, console, daemonlock, dispatch,  # noqa: E402
                     heartbeat, launcher, nightly, nightly_watch, numbers, ownerchat,
                     routing, scheduler, selfupdate, shadow, status,
                     supervisor, supervisor_sys)
from harness import (blockinfo, feature_merge, feature_sync, findings, handoff,  # noqa: E402
                     leadwake, prtext, triage, wakes)
from harness.tick_guard import Guard  # noqa: E402
from harness.brokerhost import BrokerHost, NullBroker  # noqa: E402
from harness.flow import Flow  # noqa: E402
from harness.forge import ForgeError, GhForge, job_signatures  # noqa: E402
from harness.store import PLANNER_ROLES, WORK_ROLES, Store  # noqa: E402
from harness.train import Train  # noqa: E402

NAME_PREFIX = {"engineer": "eng", "tester": "tester", "fixer": "fixer",
               "integrator": "integrator", "principal": "principal"}
PRINCIPAL_AGENT = "principal-engineer"
RETRY_LATER_S = 900.0   # a janitor that found a build container running
FIX_LIKE = ("fixer", "integrator", "principal")
ATTRIBUTION = "\U0001F916 Generated with [Claude Code](https://claude.com/claude-code)"


def make_broker(root):
    return BrokerHost(os.path.join(root, ".harness"))


class Orchestrator:
    def __init__(self, root, cfg=None, broker=None, dry_run=True,
                 fake_claude=None, clock=time.time, worktrees=None,
                 seed_fn=None, log=None, forge=None, shadow_agents=None,
                 sys_=None):
        self.root = os.path.abspath(root)
        self.cfg = cfg or scheduler.load_config(
            os.path.join(self.root, "planning", "orchestrator",
                         "orchestrator.toml"))
        self.odir = os.path.join(self.root, "planning", "orchestrator")
        self.hdir = os.path.join(self.root, ".harness")
        os.makedirs(self.odir, exist_ok=True)
        os.makedirs(os.path.join(self.hdir, "runs"), exist_ok=True)
        launcher.check_models(self.root)
        self.routing = routing.load(self.root)   # None: old behaviour
        self.clock = clock
        self.dry_run = dry_run
        self.fake_claude = fake_claude
        self.store = Store(os.path.join(self.odir, "state.json"), clock)
        self.worktrees = worktrees or launcher.GitWorktrees(self.root)
        self.forge = forge or GhForge(self.root)
        self.seed_fn = seed_fn or self._default_seed
        self.log = log or (lambda m: None)
        self.guard = Guard(self.store, clock, self.log)
        self.broker = broker or NullBroker()
        self.done_msgs = {}   # run id -> (time, body) of its `done` message
        self.undeliverable = {}   # (sender, to, type) -> (time, text)
        self.ci_polled = {}   # feature -> time of its last CI poll
        self.auto_adopt_at = 0.0   # last automatic adoption sweep
        self.pr_retry = {}    # feature -> earliest next PR-open attempt
        self.procs = {}
        self.activity = heartbeat.Activity()
        self.usage_pause_until = 0.0
        self.dry_logged = set()
        self.last_gaps = {}
        self.last_holds = {}
        self.flow = Flow(self)
        self.train = Train(self)
        # The watchdog over runs, lock, queues, PRs and disk; the tests hand
        # it a fake system (supervisor_sys.RealSys surface).
        self.supervisor = supervisor.Supervisor(
            self, sys_ or supervisor_sys.RealSys(clock=clock))
        self.attribution = ATTRIBUTION
        self.shadow = None
        self.updater = None   # enable_self_update(): a live daemon only
        if shadow_agents:   # shadow rollout: never launches, only compares
            self.dry_run = True
            self.shadow = shadow.Shadow(self._p("shadow.log"), shadow_agents,
                                        clock)

    # -- paths / switches ---------------------------------------------
    def _p(self, name):
        return os.path.join(self.odir, name)

    def kill_switch(self):
        now_ = os.path.exists(self._p("STOP.NOW"))
        return now_ or os.path.exists(self._p("STOP")), now_

    def paused(self):
        try:
            with open(self._p("paused.json"), encoding="utf-8") as f:
                return set(json.load(f))
        except (OSError, ValueError):
            return set()

    # -- broker -------------------------------------------------------
    def drain_events(self):
        """Apply broker events on this thread (the broker runs on its own)."""
        for kind, data in self.broker.drain_events():
            self.on_broker_event(kind, **data)

    def on_broker_event(self, kind, **data):
        msg = data.get("msg") or {}
        if kind == "message" and msg.get("type") == "console_reply":
            # Before the run check below: the reply must not be lost when
            # the short run has already ended.
            try:
                console.on_reply(self, msg)
            except Exception as e:
                self.store.event(None, "console_reply_failed", str(e)[:160])
        if kind == "undeliverable":
            text = (f"{data.get('type')} from {data.get('sender')} to "
                    f"'{data.get('to')}' had no recipient")
            self.undeliverable[(data.get("sender"), data.get("to"),
                                data.get("type"))] = (self.clock(), text)
            self.store.event(None, "undeliverable", text)
            return
        name = data.get("name") or (data.get("msg") or {}).get("from")
        if kind == "message" and not any(
                r["name"] == name for r in self.store.active_runs()):
            # An agent sends `done` and then writes result.json; the tick can
            # end the run before the message is drained, and it must still be
            # recorded against the task.
            last = self.store.last_run_of_name(name)
            if last:
                self.store.event(last["task_id"], "message",
                                 f"{data['msg'].get('type')} from {name}")
            return
        for r in self.store.active_runs():
            if r["name"] != name:
                continue
            if kind in ("registered", "message"):
                self.store.set_run(r["id"], last_beat=self.clock(), suspect=0)
            elif kind == "suspect":
                self.store.set_run(r["id"], suspect=1)
                self.store.event(r["task_id"], "suspect", f"{name} closed")
            elif kind == "unacked":
                self.store.event(r["task_id"], "unacked", f"{name}")
            if kind == "message":
                msg = data["msg"]
                self.store.event(r["task_id"], "message",
                                 f"{msg.get('type')} from {name}")
                if msg.get("type") == "done":
                    self.done_msgs[r["id"]] = (self.clock(),
                                               msg.get("body"))
                elif msg.get("type") == "plan_request" and                         r["role"] == "planner":
                    self.flow.on_proposal(msg.get("body"), name)

    def _tell(self, to, type, body, feature=None, task=None):
        if self.dry_run:
            return
        try:
            self.broker.tell(to, type, body, feature=feature, task=task)
        except Exception as e:  # a lost note must not stop the daemon
            self.log(f"tell {to} {type} failed: {e}")

    def _open_channel(self, t, name):
        members = {name, "lead", "architect"}
        for o in self.store.tasks():
            if o["feature_id"] == t["feature_id"]:
                members.update(x for x in (o["eng_name"], o["cur_name"]) if x)
        self.broker.open_channel(t["feature_id"], members)

    def _release(self, t):
        for name in (t["eng_name"], t["cur_name"]):
            if name:
                self.broker.release_for(name)

    # -- numbers ------------------------------------------------------
    def _default_seed(self, kind):
        return numbers.seed(kind, self.forge,
                            int(self.cfg.seeds.get(kind, 0)))

    def _reserve_numbers(self, t):
        for kind in [w for w in t["wants"].split(",") if w]:
            col = {"adr": "adr_no", "protocol": "protocol_no"}.get(kind)
            if col and t[col]:
                continue
            n = self.store.reserve(kind, t["id"], self.seed_fn(kind))
            if col:
                self.store.set_task(t["id"], **{col: n})

    # -- the tick -----------------------------------------------------
    def tick(self):
        """Every step and every item of a loop is isolated (harness/
        tick_guard): one bad result, one PR's gh error, one failing
        launch never stops the merges, the status or the launches."""
        now, g = self.clock(), self.guard
        g.step("drain_events", self.drain_events)
        stop, stop_now = self.kill_switch()
        if stop_now:
            self._kill_all("STOP.NOW")
        g.each("poll_run", self.store.active_runs(), self._poll_run, now,
               key=lambda r: r["id"], task=lambda r: r["task_id"])
        if not self.dry_run:   # every interval_s, not every tick
            g.step("supervisor", self.supervisor.tick, now)
        g.step("open_prs", self._open_pending_prs, now)
        g.step("autoadopt", autoadopt.tick, self, now)
        g.step("cli_mailbox", cli_mailbox.tick, self)
        if not self.dry_run:
            g.step("ownerchat", ownerchat.tick, self)
            g.step("console", console.tick, self, now)
        g.step("watch_prs", self._watch_prs, now)
        g.step("feature_sync", feature_sync.sync, self, now)
        # Main's nightly full build on GitHub (ADR-0061): a red opens a task.
        g.step("nightly", nightly.tick, self, now)
        g.step("flow", self.flow.tick, now)
        paused = self.paused()
        runs = self._enriched_runs(now)
        plan = g.step(
            "plan", scheduler.plan, self.store.tasks(), runs, self.cfg, now,
            stop=stop, usage_pause_until=self.usage_pause_until,
            paused=paused, hour_budget=self.cfg.max_runs_per_hour
            - self.store.runs_started_since(now - 3600, WORK_ROLES),
            planner_budget=self.cfg.max_planner_runs_per_hour
            - self.store.runs_started_since(now - 3600, PLANNER_ROLES)
        ) or scheduler.Plan()
        self.last_gaps = plan.gaps
        self.last_holds = plan.holds
        if self.shadow:
            self.shadow.record(self.store.tasks(), plan.launches,
                               self.cfg.floors)
        for role, kind, detail in leadwake.allowed(
                plan.wakes, self.store.features(),
                self.store.tasks(kind="wake"), now,
                self.cfg.lead_wake_cooldown_s):
            g.step("wake", self._ensure_wake, role, kind, detail, key=role)
        if not stop_now:
            g.each("launch", plan.launches, self._launch, now,
                   key=lambda ln: ln.task_id, task=lambda ln: ln.task_id)
        g.step("status", self._write_status, now, stop, stop_now, paused)
        return plan

    def _enriched_runs(self, now):
        out = []
        for r in self.store.active_runs():
            t = self.store.get_task(r["task_id"]) or {}
            building = scheduler.holds_build_slot(self.root, r["worktree"])
            r.update(building=building, track=t.get("track"),
                     working=heartbeat.is_working(r["last_beat"], building,
                                                  now, self.cfg))
            out.append(r)
        return out

    def _write_status(self, now, stop, stop_now, paused):
        runs = self._enriched_runs(now)
        data = status.build(
            self.store, self.cfg, now, runs, self.last_gaps,
            "dry-run" if self.dry_run else "live", stop, stop_now,
            self.usage_pause_until, paused,
            self.store.runs_started_since(now - 3600, WORK_ROLES),
            self.store.runs_started_since(now - 3600, PLANNER_ROLES),
            self.last_holds)
        data["train"] = self.train.rows()
        data["nightly"] = nightly_watch.status(self.store)
        data["train_slots"] = self.cfg.train_slots
        data["gh_paused_until"] = self.train.pause_until or None
        data["daemon_pid"] = os.getpid()   # tools/supervisor.py probes it
        data.update(self.supervisor.status())
        data["alerts"] = data.get("alerts", []) + self.guard.rows()
        try:
            status.write_atomic(self._p("status.json"), data)
        except OSError as e:   # the dashboard is reading it; next tick retries
            self.store.event("daemon", "status_write_failed", str(e)[:200])

    # -- launching ----------------------------------------------------
    def _is_dry(self, pool):
        if self.dry_run:
            return True
        lp = self.cfg.live_pools
        return lp is not None and pool not in lp

    def _name_for(self, t, kind):
        if kind == "planner":
            return t["agent"]
        if kind == "integrator" and self.cfg.max_integrators <= 1:
            return "integrator"
        if kind == "principal":
            name = f"principal-{self.store.next_counter('name_principal')}"
            self.store.set_task(t["id"], cur_name=name)
            return name
        # Several integrators at once (merge-train conflict runs) need their
        # own broker names: messages, claims and done are matched by name.
        col = "eng_name" if kind == "engineer" else "cur_name"
        name = t[col]
        if not name:
            name = f"{NAME_PREFIX[kind]}-" \
                   f"{self.store.next_counter('name_' + kind)}"
            self.store.set_task(t["id"], **{col: name})
        return name

    def _eff_kind(self, t, kind):
        """What a principal run stands in for, from the task's state."""
        if kind != "principal":
            return kind
        return scheduler.KIND_OF_STATE.get(t["state"], "fixer")

    def _agent_for(self, t, kind):
        return dispatch.agent_for(self, t, kind)

    def _launch(self, ln, now):
        t = self.store.get_task(ln.task_id)
        if self._is_dry(ln.pool):
            key = (t["id"], t["state"], ln.kind)
            if key not in self.dry_logged:
                self.dry_logged.add(key)
                self.store.event(t["id"], "dry_run_launch",
                                 f"{ln.kind} {ln.pool}")
                self.log(f"dry-run: would launch {ln.kind} for {t['id']}")
            return None
        if t["kind"] == "work":
            try:
                self._reserve_numbers(t)
            except ForgeError as e:  # never guess a number
                self.store.event(t["id"], "reserve_failed", str(e))
                self.store.set_task(t["id"], not_before=now + 60)
                return None
            t = self.store.get_task(t["id"])
        name = self._name_for(t, ln.kind)
        eff = self._eff_kind(t, ln.kind)
        files = t["files"]
        if eff in ("fixer", "integrator") and t["kind"] == "work":
            files = sorted({f for x in self._feature_tasks(t["feature_id"])
                            for f in x["files"]})
        # An adopted PR is fixed on its own branch in its own worktree: two
        # train runs touching the same file never meet, so no claim.
        adopted = self.train.info(t.get("feature_id") or "") is not None
        if t["kind"] == "work" and files and not adopted and \
                eff in ("engineer", "fixer", "integrator"):
            denied = self.broker.claim_for(name, files)
            if denied:
                self.store.event(t["id"], "claim_denied", json.dumps(denied))
                self.store.set_task(t["id"], not_before=now + 60)
                return None
        run_n = self.store.next_counter("run")
        run_id = f"{t['id']}-r{run_n}"
        run_dir = os.path.join(self.hdir, "runs", run_id)
        result_path = os.path.join(run_dir, "result.json")
        stream = os.path.join(run_dir, "stream.log")
        try:
            os.makedirs(run_dir, exist_ok=True)
            # A conflict run is the integrator's, a red check the fixer's;
            # on the train both work at the PR's head on origin.
            if t["kind"] == "work" and eff in ("fixer", "integrator") \
                    and self.train.is_adopted(t["feature_id"]):
                fb = self._feature_branch(t["feature_id"])
                wt = self.worktrees.ensure_pr(
                    self._feature_key(t["feature_id"]), fb,
                    **({"start": "origin/main"}
                       if self.train.is_bundle(t["feature_id"]) else {}))
                t["branch"], t["worktree"] = fb, wt
                self.store.set_task(t["id"], worktree=wt)
            elif t["kind"] == "work" and eff in ("integrator", "fixer"):
                fb = self._feature_branch(t["feature_id"])
                wt = self.worktrees.ensure(self._feature_key(t["feature_id"]),
                                           fb)
                t["branch"], t["worktree"] = fb, wt
                self.store.set_task(t["id"], worktree=wt)
            elif t["kind"] == "work":
                wt = self.worktrees.ensure(
                    t["id"], t["branch"],
                    **({"base": t["start_from"]} if t.get("start_from")
                       else {}))
                self.store.set_task(t["id"], worktree=wt)
                t["worktree"] = wt
            else:
                wt = self.root
            self.broker.expect(name)
            if t["feature_id"]:
                self._open_channel(t, name)
            env = launcher.run_env(name, t["id"], t["feature_id"], self.hdir,
                                   self.broker.port, self.broker.token,
                                   result_path)
            if adopted:
                env["SONORA_UNCLAIMED_OK"] = "1"
            mcp, settings = launcher.write_run_files(run_dir, env)
            prompt = launcher.build_prompt(t, run_id, name, ln.kind,
                                           result_path, self._extra(t, ln.kind))
            agent = self._agent_for(t, ln.kind)
            tier_model = dispatch.model_for(self, t, ln.kind)
            model = launcher.model_for(agent, tier_model)
            argv = launcher.build_command(
                launcher.claude_argv(self.fake_claude),
                agent, prompt, mcp, settings,
                self.cfg.max_turns, self.cfg.permission_mode,
                agents_root=self.root, model=tier_model)
            proc = launcher.spawn(argv, env, wt, stream)
        except Exception as e:  # launch failure counts like a dead run
            self.store.event(t["id"], "launch_failed", str(e))
            self._count_failure(t, f"launch failed: {e}", now)
            return None
        self.procs[run_id] = proc
        self.store.add_run(id=run_id, task_id=t["id"], role=ln.kind,
                           name=name, state=t["state"], pid=proc.pid,
                           started=now, last_beat=now, result_path=result_path,
                           stream_path=stream, worktree=wt, model=model)
        if ln.kind == "engineer" and t["state"] == "queued":
            self.store.advance(t["id"], "building")
        self.store.event(t["id"], "launched", f"{run_id} {ln.kind} {name}")
        return run_id

    def _feature_tasks(self, fid):
        return self.store.feature_tasks(fid)

    def _feature_branch(self, fid):
        f = self.store.get_feature(fid) or {}
        tasks = self._feature_tasks(fid)
        if f.get("branch"):
            return f["branch"]
        return tasks[0]["branch"] if len(tasks) == 1 else f"feat/{fid}"

    def _merge_order(self, fid):
        tasks = self._feature_tasks(fid)
        try:
            order = json.loads(self.store.get_meta(f"merge_order:{fid}", "[]"))
        except ValueError:
            order = []
        pos = {f"{fid}.{n}": i for i, n in enumerate(order)}
        return sorted(tasks, key=lambda x: pos.get(x["id"], len(pos)))

    def _feature_key(self, fid):
        """Worktree key: a lone task integrates in its own worktree (git
        allows a branch in one worktree only)."""
        tasks = self._feature_tasks(fid)
        if len(tasks) == 1 and not self.flow.is_planned(fid):
            return tasks[0]["id"]
        return f"feature-{fid}"

    def _principal_brief(self, t):
        lines = ["", "ESCALATION: you are the principal engineer, called "
                 "because ordinary fixers did not close this. Trigger: "
                 f"{t.get('escalated') or 'unknown'}.",
                 f"Branch {t['branch']}, worktree {t.get('worktree') or '-'}."]
        if t.get("note"):
            lines += ["Failure excerpt (note):", t["note"]]
        prior = [f"- {e.get('kind')}: {e.get('detail')}"
                 for e in reversed(self.store.events(t["id"], limit=40))
                 if e.get("kind") in ("result", "run_failed", "ci_red",
                                      "launched")][-12:]
        if prior:
            lines += ["Previous rounds on this task, oldest first:"] + prior
        wt = t.get("worktree")
        if wt and os.path.isdir(wt):
            try:
                out = subprocess.run(
                    ["git", "-C", wt, "log", "--oneline", "-n", "30",
                     "origin/main..HEAD"], capture_output=True, text=True,
                    timeout=10).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                out = ""
            if out:
                lines += ["Commits since main (previous fixers' work):", out]
        return "\n".join(lines)

    def _extra(self, t, kind):
        if t["kind"] == "wake":
            return self.flow.wake_context(t)
        nums = [f"{r['kind']}={r['number']}" for r in self.store.reservations()
                if r["task_id"] == t["id"]]
        extra = ("reservations: " + " ".join(nums)) if nums else ""
        bundle = t["feature_id"] and self.train.is_bundle(t["feature_id"])             and not (self.store.get_feature(t["feature_id"]) or {}).get("pr")
        if kind == "integrator" and bundle:
            extra += self.train.bundle_brief(t["feature_id"])
        elif kind == "integrator" and t["state"] == "ci_fix":
            extra += (
                "\nConflict run: the PR for this branch conflicts with main. "
                "Merge origin/main into the branch, resolve the conflicts "
                "keeping both sides' intent, rerun the narrow tests, commit. "
                "Do not push, open or merge a PR: the daemon does.")
            ti = self.train.info(t["feature_id"]) if t["feature_id"] else None
            if ti:
                extra += (
                    "\nMerge-train PR #%s (branch %s), checked out here at its "
                    "head on origin on a local train/ branch. Watch protocol, "
                    "ADR and format numbers main may have taken since: "
                    "renumber the PR's to the next free one and say so in "
                    "result.json. Never edit planning/BACKLOG.md beyond the "
                    "conflict itself." % (ti.get("pr"), ti.get("branch")))
        elif kind == "integrator":
            subs = [x["branch"] for x in self._merge_order(t["feature_id"])]
            extra += (
                "\nIntegrator run for feature %s on branch %s. Merge the "
                "sub-branches in this order: %s (skip any already merged). "
                "Run the full affected tests once, then tools/pre-pr.sh and "
                "fix what it lists. Commit; do not push, open a PR or run "
                "enqueue-pr.sh: the daemon does. In result.json put the PR "
                "text: summary, followups (the [x] BACKLOG line and "
                "follow-ups) and unverified (checks you did not run)." % (
                    t["feature_id"], self._feature_branch(t["feature_id"]),
                    ", ".join(subs)))
        if kind == "principal":
            return self._extra(t, self._eff_kind(t, kind)) + \
                self._principal_brief(t)
        if kind == "fixer":
            extra += "\nFixer brief: no scope change, no test deletion or " \
                     "weakening. Fix the failing checks named in the note, " \
                     "rerun them locally, commit; the daemon pushes."
            ti = self.train.info(t["feature_id"]) if t["feature_id"] else None
            if ti:
                extra += (
                    "\nMerge-train PR #%s (branch %s), checked out here at "
                    "its head on origin. Read why it is red before changing "
                    "anything: `gh pr checks %s`, then `gh run view --job "
                    "<id> --log-failed` for each failing job. First `git "
                    "fetch origin main && git merge origin/main` and commit "
                    "the merge if it brings anything: many reds are already "
                    "fixed on main. If the cause is outside this PR (runner "
                    "lost, apt or network failure), say so in result.json "
                    "and commit nothing more. A test that fails on main too "
                    "is still yours: this PR cannot merge while it is red, "
                    "and merging it carries the fix to main. Fix it here; "
                    "never report blocked for that. A job log still running "
                    "is readable with `gh api --allow-escape-sequences "
                    "repos/{owner}/{repo}/actions/jobs/<id>/logs`. "
                    "A conflict: `git merge "
                    "origin/main`, resolve keeping both sides' intent, run "
                    "the narrow tests. Commit on the current branch; never "
                    "push, never merge, never edit planning/BACKLOG.md." % (
                        ti.get("pr"), ti.get("branch"), ti.get("pr")))
        if kind == "tester":
            extra += "\nYou are the tester: review and run accept commands; " \
                     "never edit code."
        # The previous run's result.json is the next role's starting point.
        sibs = self._feature_tasks(t["feature_id"]) if t["feature_id"] else ()
        extra += "\n\n" + handoff.brief(self.store, t, sibs)
        return extra.strip()

    # -- polling ------------------------------------------------------
    def _poll_run(self, r, now):
        proc = self.procs.get(r["id"])
        alive = proc.poll() is None if proc else \
            launcher.pid_alive(r["pid"])
        data, err = heartbeat.read_result(r["result_path"])
        done = self.done_msgs.get(r["id"])
        if data is None and done is not None and (
                not alive or now - done[0] >= self.cfg.done_grace_s):
            # `done` message without result.json: the process is gone or
            # result.json is overdue, so the message is the result.
            data = self._result_from_done(r, done[1])
        building = scheduler.holds_build_slot(self.root, r["worktree"])
        beat = self.activity.sample(r["id"], r["worktree"],
                                    r["stream_path"], r["last_beat"], now)
        if beat != r["last_beat"]:
            self.store.set_run(r["id"], last_beat=beat)
            r["last_beat"] = beat
        verdict = heartbeat.classify(
            alive=alive, result_ok=data is not None, now=now,
            started=r["started"], last_beat=r["last_beat"], role=r["role"],
            building=building, cfg=self.cfg)
        if verdict == "ok":
            return
        if verdict == "suspect":
            if not r["suspect"]:
                self.store.set_run(r["id"], suspect=1)
                self.store.event(r["task_id"], "suspect", r["id"])
            return
        code = proc.returncode if proc else None
        if verdict == "finished":
            if alive:
                launcher.kill_tree(r["pid"])
            self._end_run(r, "finished", code)
            self._on_result(r, data, now)
            return
        if alive:
            launcher.kill_tree(r["pid"])
        task = self.store.get_task(r["task_id"])
        wait = heartbeat.usage_limit(r["stream_path"],
                                     self.cfg.pause_default_s) \
            if verdict == "dead" else None
        if wait is not None:
            self.usage_pause_until = max(self.usage_pause_until,
                                         now + wait)
            self._end_run(r, "rate_limited", code)
            self.store.set_task(task["id"], not_before=now + wait)
            self.store.event(task["id"], "usage_pause", f"{wait:.0f}s")
            return
        self._end_run(r, verdict, code)
        if task is None:  # task removed while its run was alive
            return
        heartbeat.wip_checkpoint(r["worktree"] if task["kind"] == "work"
                                 else None)
        self._count_failure(task, f"run {r['id']} {verdict}", now)

    @staticmethod
    def _result_from_done(r, body):
        body = body if isinstance(body, dict) else {"summary": str(body or "")}
        lists = {k: body[k] for k in ("findings", "followups", "unverified")
                 if isinstance(body.get(k), list)}
        status_ = body.get("status")
        return {"role": r["role"], "task": r["task_id"],
                "status": status_ if status_ in heartbeat.RESULT_STATUS
                else "done",
                "summary": str(body.get("summary") or body.get("text")
                               or "done message"), **lists}

    def _release_run_claims(self, r):
        """A run's claims die with it. Planner names ('architect') are
        reused across runs, so a claim left behind blocked every task
        touching those paths until released by hand. An engineer's name is
        its task's and its claims span QA (released by _release at task
        end). Skipped while another live run uses the same name."""
        if r["role"] == "engineer":
            return
        if any(o["name"] == r["name"] and o["id"] != r["id"]
               for o in self.store.active_runs()):
            return
        try:
            self.broker.release_for(r["name"])
        except Exception as e:  # broker already stopped on shutdown
            self.log(f"release {r['name']} failed: {e}")

    def _end_run(self, r, outcome, code):
        self.store.set_run(r["id"], ended=self.clock(), exit=code,
                           outcome=outcome)
        self.done_msgs.pop(r["id"], None)
        self._release_run_claims(r)
        self.activity.forget(r["id"])
        proc = self.procs.pop(r["id"], None)
        if proc is not None:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass

    def _kill_all(self, why):
        for r in self.store.active_runs():
            launcher.kill_tree(r["pid"])
            proc = self.procs.get(r["id"])
            if proc:
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            self._end_run(r, "killed", proc.returncode if proc else None)
            self.store.event(r["task_id"], "killed", why)

    def _count_failure(self, t, why, now):
        """A dead, stalled or failed run: relaunch the same name with
        backoff, or block after max_attempts relaunches."""
        n = t["attempts"] + 1
        self.store.event(t["id"], "run_failed", f"{why} (#{n})")
        if n > self.cfg.max_attempts and self._can_escalate(t):
            self._escalate(t, "attempts", now)
            return
        if n > self.cfg.max_attempts:
            self._block(t, f"{why}; {self.cfg.max_attempts} relaunches spent",
                        cause="infra")
            self._tell("lead", "agent_dead", {"task": t["id"], "why": why},
                       feature=t.get("feature_id"), task=t["id"])
            if t["kind"] == "work":
                self._lead_event("agent_dead", t)
            return
        back = self.cfg.backoff_s[min(n - 1, len(self.cfg.backoff_s) - 1)]
        self.store.set_task(t["id"], attempts=n, not_before=now + back)

    def _can_escalate(self, t):
        """Once per task: a failure of the escalation run itself blocks."""
        return (t["kind"] == "work" and not t.get("escalated")
                and t["state"] in ("building", "bugfix", "integrating",
                                   "ci_fix"))

    def _escalate(self, t, trigger, now, note=None):
        """Hand the task (the whole feature's tasks) to the principal
        engineer. attempts is left at the limit so that its own failure
        blocks at once."""
        for x in (self._feature_tasks(t["feature_id"]) if t["feature_id"]
                  else [t]):
            if x["state"] in ("blocked", "merged", "closed"):
                continue
            fields = {"escalated": trigger, "not_before": now,
                      "attempts": self.cfg.max_attempts}
            if note and x["id"] == t["id"]:
                fields["note"] = note
            self.store.set_task(x["id"], **fields)
        self.store.event(t["id"], "escalated", trigger)
        self.log(f"escalated {t['id']} to {PRINCIPAL_AGENT}: {trigger}")

    def _block(self, t, why, owner=None, cause=None):
        """owner: why only the owner can unblock it (a decision with no
        default, or hardware); cause: retry | conflict | infra."""
        self._release(t)
        if t["kind"] == "wake":
            self.store.set_task(t["id"], state="blocked", note=why)
            self.flow.on_wake_blocked(self.store.get_task(t["id"]))
        else:
            self.store.advance(t["id"], "blocked", note=why)
        self.store.set_task(t["id"], owner=owner, cause=cause)
        if owner:
            with open(self._p("waits-on-owner.md"), "a", encoding="utf-8") as f:
                f.write(f"\n## {t['id']} (blocked)\n- branch: {t['branch']}\n"
                        f"- reason: {why}\n- needs the owner: {owner}\n")

    def _on_agent_blocked(self, t, kind, data, now):
        why = data.get("summary", "blocked by agent")
        if blockinfo.is_retry_later(kind, why):
            # Nothing is wrong: the machine is busy. The same run is
            # offered again later, no attempt counted, nothing blocked.
            self.store.set_task(t["id"], not_before=now + RETRY_LATER_S)
            self.store.event(t["id"], "retry_later", why[:200])
            return
        owner = data.get("needs_owner")
        if not owner and triage.handle(self, t, kind, why, data, now):
            return
        self._block(t, why, owner=owner if owner else None,
                    cause=None if owner else "infra")

    # -- results ------------------------------------------------------
    def _on_result(self, r, data, now):
        t = self.store.get_task(r["task_id"])
        st, kind = data["status"], r["role"]
        self.store.event(t["id"], "result", f"{kind} {st}")
        if kind == "principal":
            kind = self._eff_kind(t, kind)
            if st != "fail":
                self._clear_escalation(t)
        for line in data.get("followups", []):
            self.store.event(t["id"], "followup", str(line))
        if st == "blocked":
            self._on_agent_blocked(t, kind, data, now)
            return
        if t["kind"] == "wake":
            if st != "done":
                self._count_failure(t, "planner run failed", now)
                return
            ok, errs = self.flow.on_wake_done(t)
            if ok:
                self.store.set_task(t["id"], state="merged")
                return
            # An invalid plan or table is never obeyed: the same role is
            # relaunched with the reasons in front of it.
            self.store.set_task(t["id"], note=(
                "Your previous file was rejected by the daemon; fix every "
                "point and write it again:\n- " + "\n- ".join(errs[:12])))
            self._count_failure(t, f"invalid file: {errs[0]}", now)
            return
        if kind == "tester":
            blocking = findings.blocking(data.get("findings"))
            if st == "fail" or blocking:
                self._bounce(t)
            else:
                self.store.advance(t["id"], "integrating")
                self._release(t)
            return
        if st == "fail":
            self._count_failure(t, "agent reported fail", now)
            return
        if kind == "engineer":
            self.store.advance(t["id"], "qa")
            # The lead reads task_done from its mailbox on its next wake;
            # waking it per task cost a third of the hourly run budget for
            # runs its charter says end in "say nothing" (2026-11-08).
            self._tell("lead", "task_done", {"task": t["id"]},
                       feature=t["feature_id"], task=t["id"])
        elif kind in ("integrator", "fixer"):
            if t["state"] == "integrating":
                self._integrated(t, data, now)
            elif t["state"] == "ci_fix":
                self._fix_done(t, now, data)

    # -- PR lifecycle -------------------------------------------------
    def _pr_count(self):
        return len({t["feature_id"] for t in self.store.tasks()
                    if t["kind"] == "work"
                    and t["state"] in ("pr_open", "ci_fix")})

    def _clear_escalation(self, t):
        for x in (self._feature_tasks(t["feature_id"]) if t["feature_id"]
                  else [t]):
            self.store.set_task(x["id"], escalated=None, attempts=0)

    def _integrated(self, t, data, now):
        """The integrator finished: the feature branch is ready. The daemon,
        not the agent, pushes and opens the one PR."""
        fid = t["feature_id"]
        self.store.set_meta(f"ready:{fid}", json.dumps({
            k: data.get(k) for k in ("summary", "followups", "unverified",
                                     "commit")}))
        for x in self._feature_tasks(fid):
            self.store.set_task(x["id"], integrated=1)
        self._open_pending_prs(now)

    def _ready_features(self):
        for f in self.store.features():
            ts = self._feature_tasks(f["id"])
            if ts and not f["pr"] and all(
                    x["state"] == "integrating" and x["integrated"]
                    for x in ts):
                yield f, ts

    def _open_pending_prs(self, now):
        if self.dry_run or self._is_dry("merge"):
            return
        for f, ts in list(self._ready_features()):
            if self._pr_count() >= self.cfg.max_open_prs:
                if self.store.get_meta(f"deferred:{f['id']}") is None:
                    self.store.set_meta(f"deferred:{f['id']}", "1")
                    self.store.event(ts[0]["id"], "pr_deferred",
                                     "max_open_prs reached; branch waits")
                continue
            if now < self.pr_retry.get(f["id"], 0):
                continue
            self.guard.step("open_pr", self._open_feature_pr, f, ts, now,
                            key=f["id"], task=ts[0]["id"])

    def _open_feature_pr(self, f, ts, now):
        fid, rep = f["id"], ts[0]
        branch = self._feature_branch(fid)
        wt = self.worktrees.ensure(self._feature_key(fid), branch)
        n = None
        try:
            ok, out = self.forge.pre_pr(wt)
            if not ok:
                self.store.event(rep["id"], "pre_pr_failed", out[-500:])
                for x in ts:
                    self.store.set_task(x["id"], integrated=0, note=(
                        "tools/pre-pr.sh failed; fix and commit:\n"
                        + out[-800:]))
                self._count_failure(rep, "pre-pr failed", now)
                return False
            self.forge.push(wt, branch)
            n = next((p["number"] for p in self.forge.open_prs()
                      if p["branch"] == branch), None)
            if n is None:
                title, body = self._pr_text(f, ts)
                n = self.forge.create_pr(branch, "main", title, body)
            # The merge owner is the watch loop (harness/feature_merge), not
            # a detached one-shot script: that script skipped PRs and nobody
            # re-queued them (2026-11-08).
            if not self.cfg.daemon_merge:
                self.forge.enqueue(n)
        except ForgeError as e:
            if n is not None:
                self._adopt_opened_pr(f, ts, rep, branch, n, now, e)
                return True
            fails = int(self.store.get_meta(f"prfail:{fid}", 0)) + 1
            self.store.set_meta(f"prfail:{fid}", fails)
            self.store.event(rep["id"], "pr_open_failed", f"{e} (#{fails})")
            self.pr_retry[fid] = now + 120
            if fails >= 4:
                self._block_feature(f, f"PR could not be opened: {e}")
            return False
        self.store.set_feature(fid, pr=n, branch=branch)
        self.store.advance_feature(fid, "pr_open", pr=n)
        for x in ts:
            self._release(x)
        self.ci_polled[fid] = now
        self.store.event(rep["id"], "pr_opened", f"#{n} {branch}")
        return True

    def _adopt_opened_pr(self, f, ts, rep, branch, n, now, err):
        """The PR exists on GitHub but a later step failed: record it so the
        train watches it (#495-#497 sat unwatched, 2026-11-07)."""
        fid = f["id"]
        self.store.event(rep["id"], "pr_adopted",
                         f"#{n} opened, later step failed: {err}")
        self.store.set_feature(fid, pr=n, branch=branch)
        self.store.advance_feature(fid, "pr_open", pr=n)
        for x in ts:
            self._release(x)
        self.ci_polled[fid] = now

    def _pr_text(self, f, ts):
        return prtext.pr_text(self.store, f, ts, ATTRIBUTION)

    def _fix_done(self, t, now, data=None):
        fid = t["feature_id"]
        f = self.store.get_feature(fid)
        branch = self._feature_branch(fid)
        try:
            if self.train.is_bundle(fid) and not f["pr"]:
                wt = t.get("worktree") or self.worktrees.path(
                    self._feature_key(fid))
                self.train.open_bundle(fid, wt, data)
            elif self.train.is_adopted(fid):
                wt = t.get("worktree") or self.worktrees.path(
                    self._feature_key(fid))
                self.forge.push_head(wt, branch)
                self.train.after_fix(fid, launcher.git_head(wt))
            else:
                self.forge.push(self.worktrees.ensure(self._feature_key(fid),
                                                      branch), branch)
                if not self.cfg.daemon_merge:
                    self.forge.enqueue(f["pr"])
        except ForgeError as e:
            self._count_failure(t, f"push after fix failed: {e}", now)
            return
        self.store.advance_feature(fid, "pr_open", fix_kind=None)
        self.ci_polled[fid] = now

    def _watch_prs(self, now):
        """Every feature with a PR, one isolated step each; a GitHub rate
        limit (step returns False) ends the sweep for this tick."""
        self.guard.each("watch_pr", self.store.features(), self._watch_pr,
                        now, key=lambda f: f["id"], task=self._rep_task)

    def _rep_task(self, f):
        """The task a feature's events are filed under (its first)."""
        ts = self._feature_tasks(f["id"])
        return ts[0]["id"] if ts else None

    def _watch_pr(self, f, now):
        ts = self._feature_tasks(f["id"])
        if not f["pr"] or not ts:
            return True
        # A blocked train PR is still watched: someone may merge or close
        # it by hand, and the board must not keep calling it blocked.
        if ts[0]["state"] != "pr_open" and not (
                ts[0]["state"] == "blocked"
                and self.train.is_adopted(f["id"])):
            return True
        if now - self.ci_polled.get(f["id"], 0) < self.cfg.ci_poll_s:
            return True
        if self.train.is_adopted(f["id"]):
            if now < self.train.pause_until:
                return False
            self.ci_polled[f["id"]] = now
            return self.train.step(f, ts, now)
        self.ci_polled[f["id"]] = now
        try:
            st = self.forge.pr_status(f["pr"])
        except ForgeError as e:
            self.store.event(ts[0]["id"], "ci_poll_failed", str(e))
            return True
        if st["state"] == "merged":
            self._merged(f, ts)
        elif st["state"] == "closed":
            self._close_feature(f, f"PR #{f['pr']} closed without merge")
        elif st["mergeable"] == "no":
            self._red(f, ts, ["merge-conflict"], "conflict")
        else:
            failing = sorted({c["name"] for c in st["checks"]
                              if c["state"] == "fail"})
            if failing:
                self._red(f, ts, failing, "ci")
            elif feature_merge.merge_if_green(self, f, ts):
                pass
            elif f.get("red_since") or f.get("principal_runs"):
                self.store.set_feature(f["id"], red_since=None,
                                       principal_runs=0)
                dispatch.clear_senior_round(self, f["id"])
        return True

    def _red(self, f, ts, jobs, fix_kind, detail=""):
        """Red check or conflict: a fixer (the track's engineer) or the
        integrator in the same tick; the same job still red after two fixes
        blocks the feature instead of spending a third agent."""
        reds = json.loads(f["job_reds"] or "{}")
        sigs = reds.pop("_sig", None)
        sigs = sigs if isinstance(sigs, dict) else {}
        # "Same red twice" means the same cause, not the same job name: a
        # job red first from a missing image, then from a stale lock file is
        # two different problems, and blocking it hid the second from its
        # fixer (#435). The cause is the job's lines in the CI excerpt.
        now_sigs = job_signatures(detail)
        for j, sig in now_sigs.items():
            if sigs.get(j) and sigs[j] != sig:
                reds[j] = 0
        # A conflict is new each time main moves: it always gets an
        # integrator, it never counts as the same red twice.
        stuck = [j for j in jobs
                 if reds.get(j, 0) >= self.cfg.principal_after_fixes
                 and j != "merge-conflict"]
        now = self.clock()
        red_since = f.get("red_since") or now
        long_red = now - red_since > self.cfg.principal_after_hours * 3600
        # The principal engineer gets one round per red spell; if the same
        # cause is still red after it, the owner decides.
        if stuck and f.get("principal_runs", 0) >= 1:
            self._block_feature(
                f, f"PR #{f['pr']}: {', '.join(stuck)} still red after "
                   f"{self.cfg.principal_after_fixes} fixes and the "
                   "principal engineer",
                owner="still red after the principal engineer: your call")
            return
        trigger = None
        # The senior tier of the area agent gets the first extra round; the
        # principal comes only when that is spent too (.claude/PROCESS.md).
        senior = bool(stuck) and not f.get("principal_runs", 0) and \
            dispatch.take_senior_round(self, f["id"])
        if f.get("principal_runs", 0) < 1:
            if stuck and senior:
                pass
            elif stuck:
                trigger = (f"same cause still red after "
                           f"{self.cfg.principal_after_fixes} fix rounds: "
                           f"{', '.join(stuck)}")
            elif long_red:
                trigger = (f"PR red for more than "
                           f"{self.cfg.principal_after_hours:g} h")
        if not trigger:
            for j in jobs:
                reds[j] = reds.get(j, 0) + 1
        sigs.update(now_sigs)
        if sigs:
            reds["_sig"] = sigs
        self.store.set_feature(
            f["id"], job_reds=json.dumps(reds), fixes=f["fixes"] + 1,
            red_since=red_since,
            principal_runs=f.get("principal_runs", 0) + (1 if trigger else 0))
        what = "conflicts with main" if fix_kind == "conflict" \
            else "red: " + ", ".join(jobs)
        note = f"PR #{f['pr']} {what}"
        if detail:
            note += "\nWhy, from the CI logs (start here):\n" + detail
        self.store.advance_feature(f["id"], "ci_fix", fix_kind=fix_kind,
                                   note=note, escalated=None)
        if trigger:
            self._escalate(ts[0], trigger, now)
        self.store.event(ts[0]["id"], "ci_red", ", ".join(jobs))

    def _merged(self, f, ts):
        self.store.advance_feature(f["id"], "merged")
        for x in ts:
            self._release(x)
        n = self.store.next_counter("merges")
        self.store.event(ts[0]["id"], "merged", f"PR #{f['pr']} (merge {n})")
        if n % self.cfg.janitor_every == 0:
            self._ensure_wake(
                "janitor", "janitor", f"after-merge-{n}",
                "Reclaim disk after merge %d: merged agent worktrees, stale "
                "build dirs, docker images and volumes of ours only. Never "
                "another project's." % n)

    def _close_feature(self, f, why):
        closed.close_feature(self, f, why)

    def _block_feature(self, f, why, cause="infra", owner=None):
        ts = [x for x in self._feature_tasks(f["id"])
              if x["state"] not in ("merged", "closed")]
        for i, x in enumerate(ts):
            if i == 0:
                self._block(x, why, cause=cause, owner=owner)
            else:
                self._release(x)
                self.store.advance(x["id"], "blocked", note=why)
                self.store.set_task(x["id"], cause=cause, owner=owner)
        self.store.set_feature(f["id"], state="blocked")

    def _bounce(self, t):
        n = t["bounces"] + 1
        self.store.set_task(t["id"], bounces=n)
        if n >= 3:
            self._block(t, "third bounce: owner decision",
                        owner="third QA bounce, no recommended default")
            return
        self.store.advance(t["id"], "bugfix")
        self._tell("lead", "qa_fail", {"task": t["id"], "bounces": n},
                   feature=t["feature_id"], task=t["id"])
        self._lead_event("qa_fail", t)
        if n == 2:
            self._ensure_wake("architect", "bug_ruling", t["id"],
                              feature=t["feature_id"])

    def _ensure_wake(self, role, kind, detail, brief=None, feature=None,
                     body=None):
        prefix = f"wake-{role}-{kind}-{detail}"
        if wakes.is_pending(self.store.tasks(kind="wake"), prefix):
            return
        n = self.store.next_counter("wake")
        wid = f"{prefix}-{n}"
        self.store.add_task(wid, None, role, kind="wake", feature_id=feature,
                            brief=brief or f"event {kind}: {detail}")
        self.store.set_task(wid, event=kind)
        self._tell(role, kind, body or {"detail": detail}, feature=feature)

    def _lead_event(self, kind, t):
        """Wake the lead for a task of a planned feature (ADR-0047 4a)."""
        fid = t.get("feature_id")
        if fid and self.flow.is_planned(fid):
            self._ensure_wake("lead", kind, t["id"], feature=fid,
                              body={"task": t["id"]})

    # -- loop ---------------------------------------------------------
    def _release_orphan_claims(self):
        """Claims persisted by a previous daemon whose run is gone."""
        live = {r["name"] for r in self.store.active_runs()}
        live.update(t["eng_name"] for t in self.store.tasks()
                    if t["eng_name"] and t["state"] not in ("merged", "closed"))
        try:
            owners = set(self.broker.claim_owners())
        except Exception:
            return
        for name in sorted(owners - live):
            try:
                self.broker.release_for(name)
            except Exception as e:
                self.log(f"release {name} failed: {e}")

    def adopt_runs(self):
        """After a daemon restart the claude processes of the previous
        daemon are still running. They stay in the store as active runs and
        _poll_runs watches them by pid and result file, so nothing is
        launched twice. The new broker is told their names; their mailboxes
        are on disk. A client that connected to the old broker cannot
        reconnect (new port and token) and finishes on result.json alone."""
        adopted = []
        self._release_orphan_claims()
        for r in self.store.active_runs():
            if launcher.pid_alive(r["pid"]):
                self.broker.expect(r["name"])
                self.store.set_run(r["id"], last_beat=self.clock(),
                                   suspect=0)
                self.store.event(r["task_id"], "adopted", r["id"])
                adopted.append(r["id"])
        return adopted

    def enable_self_update(self, argv, **kw):
        """Re-exec when origin/main changes the harness (harness/selfupdate)."""
        kw.setdefault("max_defer_s", self.cfg.self_update_max_defer_s)
        kw.setdefault("release", lambda: daemonlock.release(self.hdir))
        self.updater = selfupdate.SelfUpdate(self, argv, **kw)

    def run(self, once=False, max_ticks=None, exit_when_drained=False,
            sleep=time.sleep):
        self.broker.start()
        self.adopt_runs()
        closed.migrate(self)
        n = 0
        restart = False
        try:
            while True:
                try:
                    self.tick()
                except Exception as e:  # one bad tick must not stop the daemon
                    traceback.print_exc()
                    try:
                        self.store.event(None, "tick_error", repr(e)[:300])
                    except Exception:
                        pass
                try:
                    self.store.flush()   # heartbeats are saved here
                except Exception:
                    traceback.print_exc()
                n += 1
                try:
                    restart = bool(self.updater
                                   and self.updater.ready_or_report(
                                       self.clock()))
                except Exception:   # an update check never stops the daemon
                    traceback.print_exc()
                if restart:
                    break
                stop, _ = self.kill_switch()
                if once or (max_ticks and n >= max_ticks):
                    break
                if exit_when_drained and stop and \
                        not self.store.active_runs():
                    break
                sleep(self.cfg.tick_s)
        finally:
            self.broker.stop()
        if restart:
            self.updater.restart()

    def close(self):
        self.store.close()


def main(argv=None):
    return cli.main(argv, Orchestrator, make_broker,
                    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


if __name__ == "__main__":
    sys.exit(main())

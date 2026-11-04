#!/usr/bin/env python3
"""SonoraOS harness orchestrator daemon (ADR-0047). Standard library only.

    orchestrator.py run [--live] [--once] [--fake-claude PATH]
    orchestrator.py status | drain | stop | resume
    orchestrator.py pause --pool NAME [--resume]
    orchestrator.py add --id ID --track daw --agent rt-engineer ...

The daemon decides nothing that needs judgement. Roles decide; this file
enforces pools, caps, retries, kill switches and number reservations.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (heartbeat, launcher, numbers, scheduler, shadow,  # noqa: E402
                     status)
from harness.brokerhost import BrokerHost, NullBroker  # noqa: E402
from harness.flow import Flow  # noqa: E402
from harness.forge import ForgeError, GhForge  # noqa: E402
from harness.store import Store  # noqa: E402

NAME_PREFIX = {"engineer": "eng", "tester": "tester", "fixer": "fixer"}
ATTRIBUTION = "\U0001F916 Generated with [Claude Code](https://claude.com/claude-code)"


def make_broker(root):
    return BrokerHost(os.path.join(root, ".harness"))


class Orchestrator:
    def __init__(self, root, cfg=None, broker=None, dry_run=True,
                 fake_claude=None, clock=time.time, worktrees=None,
                 seed_fn=None, log=None, forge=None, shadow_agents=None):
        self.root = os.path.abspath(root)
        self.cfg = cfg or scheduler.load_config(
            os.path.join(self.root, "planning", "orchestrator",
                         "orchestrator.toml"))
        self.odir = os.path.join(self.root, "planning", "orchestrator")
        self.hdir = os.path.join(self.root, ".harness")
        os.makedirs(self.odir, exist_ok=True)
        os.makedirs(os.path.join(self.hdir, "runs"), exist_ok=True)
        launcher.check_models(self.root)
        self.clock = clock
        self.dry_run = dry_run
        self.fake_claude = fake_claude
        self.store = Store(os.path.join(self.odir, "state.db"), clock)
        self.worktrees = worktrees or launcher.GitWorktrees(self.root)
        self.forge = forge or GhForge(self.root)
        self.seed_fn = seed_fn or self._default_seed
        self.log = log or (lambda m: None)
        self.broker = broker or NullBroker()
        self.done_msgs = {}   # run id -> (time, body) of its `done` message
        self.ci_polled = {}   # feature -> time of its last CI poll
        self.pr_retry = {}    # feature -> earliest next PR-open attempt
        self.procs = {}
        self.activity = heartbeat.Activity()
        self.usage_pause_until = 0.0
        self.dry_logged = set()
        self.last_gaps = {}
        self.flow = Flow(self)
        self.shadow = None
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
        name = data.get("name") or (data.get("msg") or {}).get("from")
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
        now = self.clock()
        self.drain_events()
        stop, stop_now = self.kill_switch()
        if stop_now:
            self._kill_all("STOP.NOW")
        self._poll_runs(now)
        self._open_pending_prs(now)
        self._watch_prs(now)
        self.flow.tick(now)
        paused = self.paused()
        runs = self._enriched_runs(now)
        plan = scheduler.plan(
            self.store.tasks(), runs, self.cfg, now, stop=stop,
            usage_pause_until=self.usage_pause_until, paused=paused,
            hour_budget=self.cfg.max_runs_per_hour
            - self.store.runs_started_since(now - 3600))
        self.last_gaps = plan.gaps
        if self.shadow:
            self.shadow.record(self.store.tasks(), plan.launches,
                               self.cfg.floors)
        for role, kind, detail in plan.wakes:
            self._ensure_wake(role, kind, detail)
        if not stop_now:
            for ln in plan.launches:
                self._launch(ln, now)
        self._write_status(now, stop, stop_now, paused)
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
            self.store.runs_started_since(now - 3600))
        status.write_atomic(self._p("status.json"), data)

    # -- launching ----------------------------------------------------
    def _is_dry(self, pool):
        if self.dry_run:
            return True
        lp = self.cfg.live_pools
        return lp is not None and pool not in lp

    def _name_for(self, t, kind):
        if kind == "planner":
            return t["agent"]
        if kind == "integrator":
            return "integrator"
        col = "eng_name" if kind == "engineer" else "cur_name"
        name = t[col]
        if not name:
            name = f"{NAME_PREFIX[kind]}-" \
                   f"{self.store.next_counter('name_' + kind)}"
            self.store.set_task(t["id"], **{col: name})
        return name

    def _agent_for(self, t, kind):
        return {"tester": "tester", "integrator": "integrator"}.get(
            kind, t["agent"])

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
        files = t["files"]
        if ln.kind in ("fixer", "integrator") and t["kind"] == "work":
            files = sorted({f for x in self._feature_tasks(t["feature_id"])
                            for f in x["files"]})
        if t["kind"] == "work" and files and \
                ln.kind in ("engineer", "fixer", "integrator"):
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
            if t["kind"] == "work" and ln.kind in ("integrator", "fixer"):
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
            mcp, settings = launcher.write_run_files(run_dir, env)
            prompt = launcher.build_prompt(t, run_id, name, ln.kind,
                                           result_path, self._extra(t, ln.kind))
            argv = launcher.build_command(
                launcher.claude_argv(self.fake_claude),
                self._agent_for(t, ln.kind), prompt, mcp, settings,
                self.cfg.max_turns, self.cfg.permission_mode)
            proc = launcher.spawn(argv, env, wt, stream)
        except Exception as e:  # launch failure counts like a dead run
            self.store.event(t["id"], "launch_failed", str(e))
            self._count_failure(t, f"launch failed: {e}", now)
            return None
        self.procs[run_id] = proc
        self.store.add_run(id=run_id, task_id=t["id"], role=ln.kind,
                           name=name, state=t["state"], pid=proc.pid,
                           started=now, last_beat=now, result_path=result_path,
                           stream_path=stream, worktree=wt)
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

    def _extra(self, t, kind):
        if t["kind"] == "wake":
            return self.flow.wake_context(t)
        nums = [f"{r['kind']}={r['number']}" for r in self.store.reservations()
                if r["task_id"] == t["id"]]
        extra = ("reservations: " + " ".join(nums)) if nums else ""
        if kind == "integrator" and t["state"] == "ci_fix":
            extra += (
                "\nConflict run: the PR for this branch conflicts with main. "
                "Merge origin/main into the branch, resolve the conflicts "
                "keeping both sides' intent, rerun the narrow tests, commit. "
                "Do not push, open or merge a PR: the daemon does.")
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
        if kind == "fixer":
            extra += "\nFixer brief: no scope change, no test deletion or " \
                     "weakening. Fix the failing checks named in the note, " \
                     "rerun them locally, commit; the daemon pushes."
        if kind == "tester":
            extra += "\nYou are the tester: review and run accept commands; " \
                     "never edit code."
        return extra.strip()

    # -- polling ------------------------------------------------------
    def _poll_runs(self, now):
        for r in self.store.active_runs():
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
                continue
            if verdict == "suspect":
                if not r["suspect"]:
                    self.store.set_run(r["id"], suspect=1)
                    self.store.event(r["task_id"], "suspect", r["id"])
                continue
            code = proc.returncode if proc else None
            if verdict == "finished":
                if alive:
                    launcher.kill_tree(r["pid"])
                self._end_run(r, "finished", code)
                self._on_result(r, data, now)
                continue
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
                continue
            self._end_run(r, verdict, code)
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

    def _end_run(self, r, outcome, code):
        self.store.set_run(r["id"], ended=self.clock(), exit=code,
                           outcome=outcome)
        self.done_msgs.pop(r["id"], None)
        if r["role"] in ("fixer", "integrator"):
            try:
                self.broker.release_for(r["name"])
            except Exception as e:  # broker already stopped on shutdown
                self.log(f"release {r['name']} failed: {e}")
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
        if n > self.cfg.max_attempts:
            self._block(t, f"{why}; {self.cfg.max_attempts} relaunches spent")
            self._tell("lead", "agent_dead", {"task": t["id"], "why": why},
                       feature=t.get("feature_id"), task=t["id"])
            if t["kind"] == "work":
                self._lead_event("agent_dead", t)
            return
        back = self.cfg.backoff_s[min(n - 1, len(self.cfg.backoff_s) - 1)]
        self.store.set_task(t["id"], attempts=n, not_before=now + back)

    def _block(self, t, why):
        self._release(t)
        if t["kind"] == "wake":
            self.store.set_task(t["id"], state="blocked", note=why)
            self.flow.on_wake_blocked(self.store.get_task(t["id"]))
        else:
            self.store.advance(t["id"], "blocked", note=why)
        with open(self._p("waits-on-owner.md"), "a", encoding="utf-8") as f:
            f.write(f"\n## {t['id']} (blocked)\n- branch: {t['branch']}\n"
                    f"- reason: {why}\n")

    # -- results ------------------------------------------------------
    def _on_result(self, r, data, now):
        t = self.store.get_task(r["task_id"])
        st, kind = data["status"], r["role"]
        self.store.event(t["id"], "result", f"{kind} {st}")
        for line in data.get("followups", []):
            self.store.event(t["id"], "followup", str(line))
        if st == "blocked":
            self._block(t, data.get("summary", "blocked by agent"))
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
            blocking = [f for f in data.get("findings", [])
                        if f.get("severity") == "blocking"]
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
            self._tell("lead", "task_done", {"task": t["id"]},
                       feature=t["feature_id"], task=t["id"])
            self._lead_event("task_done", t)
        elif kind in ("integrator", "fixer"):
            if t["state"] == "integrating":
                self._integrated(t, data, now)
            elif t["state"] == "ci_fix":
                self._fix_done(t, now)

    # -- PR lifecycle -------------------------------------------------
    def _pr_count(self):
        return len({t["feature_id"] for t in self.store.tasks()
                    if t["kind"] == "work"
                    and t["state"] in ("pr_open", "ci_fix")})

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
            self._open_feature_pr(f, ts, now)

    def _open_feature_pr(self, f, ts, now):
        fid, rep = f["id"], ts[0]
        branch = self._feature_branch(fid)
        wt = self.worktrees.ensure(self._feature_key(fid), branch)
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
            self.forge.enqueue(n)
        except ForgeError as e:
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

    def _pr_text(self, f, ts):
        try:
            ready = json.loads(self.store.get_meta(f"ready:{f['id']}", "{}"))
        except ValueError:
            ready = {}
        summary = (ready.get("summary") or "").strip() or f["id"]
        title = f"{f['id']}: {summary.splitlines()[0]}"[:100]
        if f["partial"]:
            title = "[partial] " + title
        nums = [f"{r['kind']} {r['number']}" for r in
                self.store.reservations()
                if r["task_id"] in {x["id"] for x in ts}]
        lines = ["## Summary", summary, "", "## Tasks"]
        lines += [f"- {x['id']} ({x['agent']})" for x in ts]
        if nums:
            lines += ["", "## Reserved numbers", ", ".join(nums)]
        for head, key in (("Follow-ups", "followups"),
                          ("Unverified", "unverified")):
            items = ready.get(key) or []
            if items:
                lines += ["", f"## {head}"] + [f"- {i}" for i in items]
        lines += ["", ATTRIBUTION]
        return title, "\n".join(lines)

    def _fix_done(self, t, now):
        fid = t["feature_id"]
        f = self.store.get_feature(fid)
        branch = self._feature_branch(fid)
        try:
            self.forge.push(self.worktrees.ensure(self._feature_key(fid),
                                                  branch), branch)
            self.forge.enqueue(f["pr"])
        except ForgeError as e:
            self._count_failure(t, f"push after fix failed: {e}", now)
            return
        self.store.advance_feature(fid, "pr_open", fix_kind=None)
        self.ci_polled[fid] = now

    def _watch_prs(self, now):
        for f in self.store.features():
            ts = self._feature_tasks(f["id"])
            if not f["pr"] or not ts or ts[0]["state"] != "pr_open":
                continue
            if now - self.ci_polled.get(f["id"], 0) < self.cfg.ci_poll_s:
                continue
            self.ci_polled[f["id"]] = now
            try:
                st = self.forge.pr_status(f["pr"])
            except ForgeError as e:
                self.store.event(ts[0]["id"], "ci_poll_failed", str(e))
                continue
            if st["state"] == "merged":
                self._merged(f, ts)
            elif st["state"] == "closed":
                self._block_feature(f, f"PR #{f['pr']} closed without merge")
            elif st["mergeable"] == "no":
                self._red(f, ts, ["merge-conflict"], "conflict")
            else:
                failing = sorted({c["name"] for c in st["checks"]
                                  if c["state"] == "fail"})
                if failing:
                    self._red(f, ts, failing, "ci")

    def _red(self, f, ts, jobs, fix_kind):
        """Red check or conflict: a fixer (the track's engineer) or the
        integrator in the same tick; the same job still red after two fixes
        blocks the feature instead of spending a third agent."""
        reds = json.loads(f["job_reds"] or "{}")
        stuck = [j for j in jobs if reds.get(j, 0) >= 2]
        if stuck:
            self._block_feature(
                f, f"PR #{f['pr']}: {', '.join(stuck)} still red after two "
                   "fixes")
            return
        for j in jobs:
            reds[j] = reds.get(j, 0) + 1
        self.store.set_feature(f["id"], job_reds=json.dumps(reds),
                               fixes=f["fixes"] + 1)
        what = "conflicts with main" if fix_kind == "conflict" \
            else "red: " + ", ".join(jobs)
        self.store.advance_feature(f["id"], "ci_fix", fix_kind=fix_kind,
                                   note=f"PR #{f['pr']} {what}")
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

    def _block_feature(self, f, why):
        ts = [x for x in self._feature_tasks(f["id"])
              if x["state"] != "merged"]
        for i, x in enumerate(ts):
            if i == 0:
                self._block(x, why)
            else:
                self._release(x)
                self.store.advance(x["id"], "blocked", note=why)
        self.store.set_feature(f["id"], state="blocked")

    def _bounce(self, t):
        n = t["bounces"] + 1
        self.store.set_task(t["id"], bounces=n)
        if n >= 3:
            self._block(t, "third bounce: owner decision")
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
        for w in self.store.tasks(kind="wake"):
            if w["id"].startswith(prefix) and w["state"] in ("queued",
                                                             "blocked"):
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
    def adopt_runs(self):
        """After a daemon restart the claude processes of the previous
        daemon are still running. They stay in the store as active runs and
        _poll_runs watches them by pid and result file, so nothing is
        launched twice. The new broker is told their names; their mailboxes
        are on disk. A client that connected to the old broker cannot
        reconnect (new port and token) and finishes on result.json alone."""
        adopted = []
        for r in self.store.active_runs():
            if launcher.pid_alive(r["pid"]):
                self.broker.expect(r["name"])
                self.store.set_run(r["id"], last_beat=self.clock(),
                                   suspect=0)
                self.store.event(r["task_id"], "adopted", r["id"])
                adopted.append(r["id"])
        return adopted

    def run(self, once=False, max_ticks=None, exit_when_drained=False,
            sleep=time.sleep):
        self.broker.start()
        self.adopt_runs()
        n = 0
        try:
            while True:
                self.tick()
                n += 1
                stop, _ = self.kill_switch()
                if once or (max_ticks and n >= max_ticks):
                    break
                if exit_when_drained and stop and \
                        not self.store.active_runs():
                    break
                sleep(self.cfg.tick_s)
        finally:
            self.broker.stop()

    def close(self):
        self.store.close()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="orchestrator.py")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--live", action="store_true",
                   help="launch for real (default is dry-run)")
    r.add_argument("--fake-claude")
    r.add_argument("--once", action="store_true")
    r.add_argument("--exit-when-drained", action="store_true")
    r.add_argument("--no-broker", action="store_true")
    r.add_argument("--shadow", action="store_true",
                   help="dry run that logs differences from the live agents")
    r.add_argument("--shadow-agents",
                   help="JSON file of the agents the coordinator runs")
    q = sub.add_parser("import-queue")
    q.add_argument("path")
    q.add_argument("--wants", default="adr")
    for c in ("status", "drain", "stop", "resume"):
        sub.add_parser(c)
    p = sub.add_parser("pause")
    p.add_argument("--pool", required=True)
    p.add_argument("--resume", action="store_true")
    a = sub.add_parser("add")
    a.add_argument("--id", required=True)
    a.add_argument("--track", required=True)
    a.add_argument("--agent", required=True)
    a.add_argument("--files", default="")
    a.add_argument("--brief", default="")
    a.add_argument("--depends-on", default="")
    a.add_argument("--wants", default="")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    odir = os.path.join(root, "planning", "orchestrator")
    os.makedirs(odir, exist_ok=True)

    def touch(name):
        open(os.path.join(odir, name), "a").close()

    if args.cmd == "drain":
        touch("STOP")
    elif args.cmd == "stop":
        touch("STOP")
        touch("STOP.NOW")
    elif args.cmd == "resume":
        for n in ("STOP", "STOP.NOW"):
            try:
                os.remove(os.path.join(odir, n))
            except FileNotFoundError:
                pass
    elif args.cmd == "status":
        try:
            with open(os.path.join(odir, "status.json"),
                      encoding="utf-8") as f:
                print(f.read())
        except FileNotFoundError:
            print("no status.json yet; is the daemon running?")
            return 1
    elif args.cmd == "pause":
        path = os.path.join(odir, "paused.json")
        try:
            with open(path, encoding="utf-8") as f:
                cur = set(json.load(f))
        except (OSError, ValueError):
            cur = set()
        cur = cur - {args.pool} if args.resume else cur | {args.pool}
        status.write_atomic(path, sorted(cur))
    elif args.cmd == "add":
        o = Orchestrator(root)
        o.store.add_task(
            args.id, args.track, args.agent,
            files=[x for x in args.files.split(",") if x], brief=args.brief,
            depends_on=[x for x in args.depends_on.split(",") if x],
            wants=[x for x in args.wants.split(",") if x])
        o.close()
    elif args.cmd == "import-queue":
        o = Orchestrator(root)
        with open(args.path, encoding="utf-8") as f:
            made = o.flow.import_queue(
                f.read(), wants=[x for x in args.wants.split(",") if x])
        o.close()
        print(f"imported {len(made)} feature(s): {', '.join(made)}")
    elif args.cmd == "run":
        if args.shadow and not args.shadow_agents:
            ap.error("--shadow needs --shadow-agents FILE")
        try:
            broker = NullBroker() if args.no_broker else make_broker(root)
            o = Orchestrator(root, broker=broker, dry_run=not args.live,
                             fake_claude=args.fake_claude,
                             shadow_agents=args.shadow_agents
                             if args.shadow else None,
                             log=lambda m: print(m, flush=True))
        except launcher.ModelError as e:
            print(f"orchestrator: refusing to start: {e}", file=sys.stderr)
            return 2
        try:
            o.run(once=args.once, exit_when_drained=args.exit_when_drained)
        except KeyboardInterrupt:
            pass
        finally:
            o.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

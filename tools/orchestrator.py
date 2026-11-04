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

from harness import heartbeat, launcher, scheduler, status  # noqa: E402
from harness.store import Store  # noqa: E402

NAME_PREFIX = {"engineer": "eng", "tester": "tester", "fixer": "fixer"}


class NullBroker:
    """Used when no broker module is present (dry-run, tests)."""
    port = 0

    def __init__(self):
        self.on_event = None

    def start(self):
        pass

    def stop(self):
        pass

    def send_as_daemon(self, to, type, body):
        pass


def make_broker(root):
    try:
        from harness.broker import Broker
    except ImportError:
        print("orchestrator: harness.broker not available; running without "
              "a broker", file=sys.stderr)
        return NullBroker()
    return Broker(os.path.join(root, ".harness"))


class Orchestrator:
    def __init__(self, root, cfg=None, broker=None, dry_run=True,
                 fake_claude=None, clock=time.time, worktrees=None,
                 seed_fn=None, log=None):
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
        self.seed_fn = seed_fn or self._default_seed
        self.log = log or (lambda m: None)
        self.broker = broker or NullBroker()
        if hasattr(self.broker, "set_event_callback"):
            self.broker.set_event_callback(self.on_broker_event)
        else:
            self.broker.on_event = self.on_broker_event
        self.procs = {}
        self.activity = heartbeat.Activity()
        self.usage_pause_until = 0.0
        self.dry_logged = set()
        self.last_gaps = {}

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
    def on_broker_event(self, kind, *args, **kw):
        name = next((a for a in args if isinstance(a, str)), kw.get("name"))
        for r in self.store.active_runs():
            if r["name"] != name:
                continue
            if kind in ("registered", "message"):
                self.store.set_run(r["id"], last_beat=self.clock(), suspect=0)
            elif kind == "suspect":
                self.store.set_run(r["id"], suspect=1)
                self.store.event(r["task_id"], "suspect", f"{name} closed")

    def _tell(self, to, type, body):
        if not self.dry_run:
            self.broker.send_as_daemon(to, type, body)

    # -- numbers ------------------------------------------------------
    def _default_seed(self, kind):
        seed = int(self.cfg.seeds.get(kind, 0))
        if kind != "adr":
            return seed
        nums = [seed]
        for ref in ("origin/main", "main"):
            r = subprocess.run(["git", "-C", self.root, "ls-tree", "-r",
                                "--name-only", ref, "brain/decisions"],
                               capture_output=True, text=True)
            if r.returncode == 0:
                nums += [int(m) for m in re.findall(r"ADR-(\d+)", r.stdout)]
                break
        try:
            r = subprocess.run(["gh", "pr", "list", "--json", "files",
                                "--limit", "100"], cwd=self.root, timeout=30,
                               capture_output=True, text=True)
            if r.returncode == 0:
                nums += [int(m) for m in re.findall(r"ADR-(\d+)", r.stdout)]
        except (OSError, subprocess.SubprocessError):
            pass
        return max(nums)

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
        stop, stop_now = self.kill_switch()
        if stop_now:
            self._kill_all("STOP.NOW")
        self._poll_runs(now)
        paused = self.paused()
        runs = self._enriched_runs(now)
        plan = scheduler.plan(
            self.store.tasks(), runs, self.cfg, now, stop=stop,
            usage_pause_until=self.usage_pause_until, paused=paused,
            hour_budget=self.cfg.max_runs_per_hour
            - self.store.runs_started_since(now - 3600))
        self.last_gaps = plan.gaps
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
            self._reserve_numbers(t)
            t = self.store.get_task(t["id"])
        name = self._name_for(t, ln.kind)
        run_n = self.store.next_counter("run")
        run_id = f"{t['id']}-r{run_n}"
        run_dir = os.path.join(self.hdir, "runs", run_id)
        result_path = os.path.join(run_dir, "result.json")
        stream = os.path.join(run_dir, "stream.log")
        try:
            os.makedirs(run_dir, exist_ok=True)
            if t["kind"] == "work":
                wt = self.worktrees.ensure(t["id"], t["branch"])
                self.store.set_task(t["id"], worktree=wt)
                t["worktree"] = wt
            else:
                wt = self.root
            token = launcher.new_token()
            port = getattr(self.broker, "port", 0)
            if hasattr(self.broker, "expect_agent"):
                self.broker.expect_agent(name, token, t["id"],
                                         t["feature_id"])
            env = launcher.run_env(name, t["id"], t["feature_id"], self.hdir,
                                   port, token, result_path)
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

    def _extra(self, t, kind):
        nums = [f"{r['kind']}={r['number']}" for r in self.store.reservations()
                if r["task_id"] == t["id"]]
        extra = ("reservations: " + " ".join(nums)) if nums else ""
        if kind == "fixer":
            extra += "\nFixer brief: no scope change, no test deletion or " \
                     "weakening."
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

    def _end_run(self, r, outcome, code):
        self.store.set_run(r["id"], ended=self.clock(), exit=code,
                           outcome=outcome)
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
            self._tell("lead", "agent_dead", {"task": t["id"], "why": why})
            return
        back = self.cfg.backoff_s[min(n - 1, len(self.cfg.backoff_s) - 1)]
        self.store.set_task(t["id"], attempts=n, not_before=now + back)

    def _block(self, t, why):
        if t["kind"] == "wake":
            self.store.set_task(t["id"], state="blocked", note=why)
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
            if st == "done":
                self.store.set_task(t["id"], state="merged")
            else:
                self._count_failure(t, "planner run failed", now)
            return
        if kind == "tester":
            blocking = [f for f in data.get("findings", [])
                        if f.get("severity") == "blocking"]
            if st == "fail" or blocking:
                self._bounce(t)
            else:
                self.store.advance(t["id"], "integrating")
            return
        if st == "fail":
            self._count_failure(t, "agent reported fail", now)
            return
        if kind == "engineer":
            self.store.advance(t["id"], "qa")
            self._tell("lead", "task_done", {"task": t["id"]})
        elif kind in ("integrator", "fixer"):
            if data.get("pr"):
                self.store.set_task(t["id"], pr=int(data["pr"]))
            self.store.advance(t["id"], "pr_open")

    def _bounce(self, t):
        n = t["bounces"] + 1
        self.store.set_task(t["id"], bounces=n)
        if n >= 3:
            self._block(t, "third bounce: owner decision")
            return
        self.store.advance(t["id"], "bugfix")
        self._tell("lead", "qa_fail", {"task": t["id"], "bounces": n})
        if n == 2:
            self._ensure_wake("architect", "bug_ruling", t["id"])

    def _ensure_wake(self, role, kind, detail):
        prefix = f"wake-{role}-{kind}-{detail}"
        for w in self.store.tasks(kind="wake"):
            if w["id"].startswith(prefix) and w["state"] in ("queued",
                                                             "blocked"):
                return
        n = self.store.next_counter("wake")
        self.store.add_task(f"{prefix}-{n}", None, role, kind="wake",
                            brief=f"event {kind}: {detail}")
        self._tell(role, kind, {"detail": detail})

    # -- loop ---------------------------------------------------------
    def run(self, once=False, max_ticks=None, exit_when_drained=False,
            sleep=time.sleep):
        self.broker.start()
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
    elif args.cmd == "run":
        try:
            broker = NullBroker() if args.no_broker else make_broker(root)
            o = Orchestrator(root, broker=broker, dry_run=not args.live,
                             fake_claude=args.fake_claude,
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

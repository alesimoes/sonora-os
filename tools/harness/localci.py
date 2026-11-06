"""The local build-and-test gate (owner, 2026-11-05).

GitHub's free runners have 4 vCPUs: the heavy build and test jobs left
.github/workflows/ci.yml and run here, through tools/local-ci.sh, on the
exact commit that is about to become a PR or be merged, merged with
today's main. A head is judged once; the result is kept in the store and
posted on the commit as the `local-ci` status, so the PR shows it.

Each run gets one of `local_ci_slots` checkouts (.harness/ci/sonora-ci-N,
git worktrees of this repository). They are reused, and so is the build
volume tools/build-ui.sh keys on the checkout's name: a build after the
first is incremental.

One builder, in batches (owner, 2026-11-05: one machine gathers everything
and does the heavy build). The PRs the merge train finds ready (light checks
green, no conflict) are not built one by one: start() queues the head, and a
minute after the first one arrived poll() merges main and every queued head
into ONE candidate and runs ONE local-ci on it. Green: every head in it is
green and the train merges them all. Red: the batch is split in halves, each
built on its own, down to a single head; that head is the culprit, its `fail`
result carries the log for a fixer, and the heads of the green halves merge.
A head that conflicts with main or with another one is dropped from the batch
(a conflict result, so the fixer merges main). Pre-PR runs of feature
branches (start(..., pre=True)) take the checkouts one at a time, after the
batches. Only the orchestrator (this module, from its tick) starts a heavy
run. The machine-wide lock (tools/build-slot.sh) orders the steps of runs
that overlap: SONORA_HEAVY_PRIORITY is 0 for a batch, 1 for main's full run
and 2 for a pre-PR run. builder_status() is the dashboard's one "builder"
card. The run is a detached shell that writes rc and
summary files; the daemon only polls them, so a daemon restart loses
nothing.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

TIMEOUT_S = 3 * 3600
BATCH_SETTLE_S = 60   # PRs ready within a minute share one build
EXCERPT_LINES = 30
ERROR_LINE = re.compile(
    r"error[: ]|\*\*\*Failed|FAIL|Timeout|undefined reference|"
    r"Segmentation|ThreadSanitizer|AddressSanitizer|RealtimeSanitizer",
    re.I)


def bash_exe():
    """Git's bash on Windows: a bare "bash" there can be WSL's
    (System32/bash.exe), which reads none of the C:/ paths given to it."""
    if sys.platform != "win32":
        return "bash"
    git = shutil.which("git")
    if git:
        base = os.path.dirname(os.path.dirname(git))
        for rel in (("bin", "bash.exe"), ("usr", "bin", "bash.exe")):
            cand = os.path.join(base, *rel)
            if os.path.exists(cand):
                return cand
    return os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                        "Git", "bin", "bash.exe")


def passed(rc, summary):
    """A run passes only when the script said so. rc alone is not enough:
    a local-ci.sh killed on Windows leaves the wrapper writing 0 (2026-11-05:
    a batch with a red tree step was taken as green and merged)."""
    if rc != 0 or re.search(r"^FAIL ", summary, re.M):
        return False
    return bool(re.search(r"^local-ci: (all passed|nothing to build)",
                          summary, re.M))


def _git(args, cwd, timeout=600):
    return subprocess.run(["git"] + args, cwd=cwd, capture_output=True,
                          text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


class LocalCI:
    def __init__(self, orch, clock=time.time):
        self.o = orch
        self.clock = clock
        self.base = os.path.join(orch.root, ".harness", "ci")

    # -- state ------------------------------------------------------------
    def result(self, sha):
        raw = self.o.store.get_meta(f"localci:{sha}")
        try:
            return json.loads(raw) if raw else None
        except ValueError:
            return None

    def _set(self, sha, **fields):
        r = self.result(sha) or {}
        r.update(fields)
        self.o.store.set_meta(f"localci:{sha}", json.dumps(r))

    def _running(self):
        out = []
        for k, v in list(self.o.store._d["meta"].items()):
            if k.startswith("localci:"):
                try:
                    r = json.loads(v)
                except ValueError:
                    continue
                # a queued head and a batch member have no run of their own
                if r.get("state") == "running" and r.get("dir") \
                        and not r.get("batch"):
                    out.append((k[len("localci:"):], r))
        return out

    def _queue(self, name):
        try:
            return json.loads(self.o.store.get_meta("localci-q:" + name)
                              or "[]")
        except ValueError:
            return []

    def _set_queue(self, name, items):
        self.o.store.set_meta("localci-q:" + name, json.dumps(items))

    def _free_slot(self):
        busy = {r.get("slot") for _, r in self._running()}
        return next((k for k in range(max(1, self.o.cfg.local_ci_slots))
                     if k not in busy), None)

    # -- running ----------------------------------------------------------
    def start(self, sha, full=False, pre=False):
        """Asks for a head to be built and tested. True when the request is
        accepted (the result says running until the batch it joined ends);
        main's full run (full=True) is False when no checkout is free."""
        if self.result(sha):
            return True
        if full:
            slot = self._free_slot()
            return slot is not None and self._launch(sha, [sha], slot, True)
        q = self._queue("pool")
        if not any(e["head"] == sha for e in q):
            q.append({"head": sha, "since": self.clock(), "pre": pre})
            self._set_queue("pool", q)
        self._set(sha, state="running", queued=True, pre=pre)
        return True

    def _launch_queued(self):
        """Fills the free checkouts: bisect halves first, then one batch of
        every ready PR, then the pre-PR runs of feature branches."""
        while True:
            slot = self._free_slot()
            if slot is None:
                return
            groups, pool = self._queue("groups"), self._queue("pool")
            ready = [e for e in pool if not e.get("pre")]
            if groups:
                heads, prio = groups.pop(0), 0
                self._set_queue("groups", groups)
            elif ready and self.clock() - min(
                    e["since"] for e in ready) >= BATCH_SETTLE_S:
                heads, prio = [e["head"] for e in ready], 0
                self._set_queue("pool", [e for e in pool if e.get("pre")])
            elif not ready and pool:
                first = pool[0]
                heads, prio = [first["head"]], 2
                self._set_queue("pool", pool[1:])
            else:
                return
            self._launch(None, heads, slot, False, prio)

    def _launch(self, key, heads, slot, full, prio=1):
        """Merges heads into today's main in checkout `slot` and starts one
        detached run on the result; key is the sha of main's full run."""
        wt = os.path.join(self.base, f"sonora-ci-{slot}")
        key = key or "b" + hashlib.sha1(
            ",".join(heads).encode()).hexdigest()[:12]
        logdir = os.path.join(self.base, "logs", key[:12])
        os.makedirs(logdir, exist_ok=True)
        why, dropped, merged = self._checkout(wt, heads)
        for h, reason in dropped:
            self._set(h, state="fail", summary=reason, excerpt=reason,
                      queued=False, ended=self.clock())
            self._post(h, "failure", reason)
        if why or not merged:
            for h in merged or heads:
                if not any(h == d[0] for d in dropped):
                    self._set(h, state="fail", summary=why, excerpt=why,
                              queued=False, ended=self.clock())
                    self._post(h, "failure", why or "nothing to build")
            return True
        for f in ("rc", "summary"):
            try:
                os.remove(os.path.join(logdir, f))
            except OSError:
                pass
        # The daemon's own copy of the script judges every head the same
        # way, including heads from before the script existed.
        script = os.path.join(self.o.root, "tools", "local-ci.sh")
        cmd = ('bash "$S"%s > "$D/summary" 2>&1; echo $? > "$D/rc"'
               % (" --full" if full else ""))
        env = dict(os.environ, D=logdir.replace("\\", "/"),
                   S=script.replace("\\", "/"),
                   SONORA_LOCAL_CI_ROOT=wt.replace("\\", "/"),
                   SONORA_LOCAL_CI_DIR=logdir.replace("\\", "/"),
                   SONORA_HEAVY_PRIORITY=str(prio),
                   # a batch that fails is split anyway; stop at its first red
                   SONORA_LOCAL_CI_FAIL_FAST="1" if len(merged) > 1 else "0",
                   # a batch keeps the machine from its first step to its
                   # last (tools/local-ci.sh)
                   SONORA_LOCAL_CI_HOLD_LOCK="1" if prio == 0 else "0")
        kw = {}
        if sys.platform == "win32":
            kw["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                                   | subprocess.DETACHED_PROCESS)
        else:
            kw["start_new_session"] = True
        subprocess.Popen([bash_exe(), "-c", cmd], cwd=wt, env=env,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, **kw)
        self._set(key, state="running", slot=slot, dir=logdir,
                  started=self.clock(), full=full, heads=merged, prio=prio)
        for h in merged:
            if h != key:
                self._set(h, state="running", queued=False, batch=key)
        for h in merged:
            self._post(h, "pending", "build and tests running on the "
                       "local machine")
        return True

    def _checkout(self, wt, heads):
        """(error, [(head, why dropped)], merged heads): today's main with
        every head merged in; one that conflicts is dropped, not the batch."""
        root = self.o.root
        if _git(["fetch", "-q", "origin", "main"] + list(heads),
                root).returncode:
            # a local branch's commit is already in the object store
            heads = [h for h in heads
                     if not _git(["cat-file", "-e", h], root).returncode]
            _git(["fetch", "-q", "origin", "main"], root)
            if not heads:
                return "git fetch: heads are not in the repository", [], []
        main = _git(["rev-parse", "origin/main"], root).stdout.strip()
        if not os.path.isdir(wt):
            os.makedirs(self.base, exist_ok=True)
            r = _git(["worktree", "add", "-f", "--detach", wt, main], root)
            if r.returncode:
                return f"git worktree add: {r.stderr.strip()[:200]}", [], []
        for args in (["merge", "--abort"], ["reset", "-q", "--hard"],
                     ["checkout", "-q", "-f", "--detach", main],
                     ["clean", "-fdxq", "-e", "build-ui-container",
                      "-e", "build-container"]):
            r = _git(args, wt)
            if r.returncode and args[0] in ("checkout", "clean"):
                return f"git {args[0]}: {r.stderr.strip()[:200]}", [], []
        dropped, merged = [], []
        for h in heads:
            if h == main:
                merged.append(h)
                continue
            r = _git(["-c", "user.name=local-ci", "-c",
                      "user.email=local-ci@localhost", "merge", "-q",
                      "--no-edit", h], wt)
            if r.returncode:
                _git(["merge", "--abort"], wt)
                dropped.append((h, "conflicts with main (or with another PR "
                                "of the batch)"))
            else:
                merged.append(h)
        return None, dropped, merged

    def poll(self):
        """Finishes the runs whose shell wrote rc; posts their status; fills
        the free checkouts from the queues."""
        for sha, r in self._running():
            d = r.get("dir") or ""
            rc_path = os.path.join(d, "rc")
            if not os.path.exists(rc_path):
                if self.clock() - r.get("started", 0) > TIMEOUT_S:
                    self._finish(sha, False, "timed out after 3 h", "")
                continue
            try:
                rc = int(open(rc_path).read().strip() or 1)
                summary = open(os.path.join(d, "summary"),
                               encoding="utf-8", errors="replace").read()
            except (OSError, ValueError):
                continue
            self._finish(sha, passed(rc, summary), summary, d)
        self._launch_queued()

    def _finish(self, key, ok, summary, d):
        excerpt = summary.strip()
        if not ok and d:
            for m in re.finditer(r"^FAIL (\S+)", summary, re.M):
                try:
                    lines = open(os.path.join(d, m.group(1) + ".log"),
                                 encoding="utf-8",
                                 errors="replace").read().splitlines()
                except OSError:
                    continue
                hits = [ln for ln in lines if ERROR_LINE.search(ln)]
                excerpt += f"\n[{m.group(1)}]\n" + "\n".join(
                    (hits or lines)[-EXCERPT_LINES:])
        heads = (self.result(key) or {}).get("heads") or [key]
        self._set(key, state="pass" if ok else "fail", summary=summary[-2000:],
                  excerpt=excerpt[-4000:], ended=self.clock())
        if not ok and len(heads) > 1:
            # Which PR broke it: each half is built on its own; a half that
            # is green merges, a head left red alone is the culprit.
            mid = len(heads) // 2
            self._set_queue("groups", self._queue("groups")
                            + [heads[:mid], heads[mid:]])
            for h in heads:
                self._set(h, state="running", queued=True, batch=None)
            self.o.store.event("local-ci", "local_ci_bisect",
                               f"{key} {len(heads)} PRs red, split")
            return
        last = [ln for ln in summary.strip().splitlines() if ln][-1:] or [""]
        for h in heads:
            if h != key:
                self._set(h, state="pass" if ok else "fail",
                          summary=summary[-2000:], excerpt=excerpt[-4000:],
                          queued=False, batch=None, dir=d, ended=self.clock())
            self._post(h, "success" if ok else "failure", last[0])
        self.o.store.event("local-ci", "local_ci_" + ("pass" if ok else "fail"),
                           f"{heads[0][:8]} {last[0][:120]}")

    def builder_status(self, now=None):
        """The dashboard's one "builder" card: the batch running, the step
        it is on (the newest step log), how long, what waits behind it and
        the last finished run with its log directory."""
        now = now or self.clock()
        cur = None
        for key, r in self._running():
            step = None
            try:
                logs = [os.path.join(r["dir"], f) for f in
                        os.listdir(r["dir"]) if f.endswith(".log")]
                if logs:
                    step = os.path.basename(max(logs, key=os.path.getmtime))[:-4]
            except OSError:
                pass
            cur = {"key": key, "heads": r.get("heads") or [key],
                   "full": bool(r.get("full")), "priority": r.get("prio"),
                   "step": step, "started": r.get("started"),
                   "elapsed_s": int(now - r.get("started", now)),
                   "dir": r.get("dir")}
            break
        last = None
        for k, v in list(self.o.store._d["meta"].items()):
            if not k.startswith("localci:"):
                continue
            try:
                r = json.loads(v)
            except ValueError:
                continue
            if r.get("heads") and r.get("ended") and (
                    last is None or r["ended"] > last["ended"]):
                last = {"key": k[len("localci:"):], "heads": r["heads"],
                        "state": r.get("state"), "ended": r["ended"],
                        "dir": r.get("dir"),
                        "summary": (r.get("summary") or "").strip()
                        .splitlines()[-1:] or [""]}
        pool = self._queue("pool")
        return {"running": cur, "last": last,
                "queue": {"batch": [e["head"] for e in pool
                                    if not e.get("pre")],
                          "pre": [e["head"] for e in pool if e.get("pre")],
                          "bisect": self._queue("groups")}}

    def _post(self, sha, state, desc):
        try:
            self.o.forge.set_status(sha, state, "local-ci", desc[:140])
        except Exception as e:   # the gate stands without the PR badge
            self.o.store.event("local-ci", "status_failed", str(e)[:200])

    # -- main -------------------------------------------------------------
    def check_main(self, every_s=600):
        """main after each merge: every step (--full), once per main head.
        A red main is an event and the `local-ci` status on its commit; the
        next PR gates merge with it, so their failures say so too."""
        now = self.clock()
        if now - getattr(self, "_main_checked", 0) < every_s:
            return
        # A batch waiting for a checkout goes first; main's full run takes
        # the slot for an hour.
        if self._queue("pool") or self._queue("groups"):
            return
        self._main_checked = now
        if _git(["fetch", "-q", "origin", "main"], self.o.root).returncode:
            return
        sha = _git(["rev-parse", "origin/main"], self.o.root).stdout.strip()
        if sha and self.result(sha) is None:
            self.start(sha, full=True)

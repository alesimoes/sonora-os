"""The local build-and-test gate (owner, 2026-11-05).

GitHub's free runners have 4 vCPUs: the heavy build and test jobs left
.github/workflows/ci.yml and run here, through tools/local-ci.sh, on the
exact commit that is about to become a PR or be merged, merged with
today's main. A head is judged once; the result is kept in the store and
posted on the commit as the `local-ci` status, so the PR shows it.

Each run gets one of `local_ci_slots` checkouts (.harness/ci/sonora-ci-N,
git worktrees of this repository). They are reused, and so is the build
volume tools/build-ui.sh keys on the checkout's name: a build after the
first is incremental. The run is a detached shell that writes rc and
summary files; the daemon only polls them, so a daemon restart loses
nothing.
"""
import json
import os
import re
import subprocess
import sys
import time

TIMEOUT_S = 3 * 3600
EXCERPT_LINES = 30
ERROR_LINE = re.compile(
    r"error[: ]|\*\*\*Failed|FAIL|Timeout|undefined reference|"
    r"Segmentation|ThreadSanitizer|AddressSanitizer|RealtimeSanitizer",
    re.I)


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
                if r.get("state") == "running":
                    out.append((k[len("localci:"):], r))
        return out

    # -- running ----------------------------------------------------------
    def start(self, sha, full=False):
        """Starts a run for sha in a free slot. False when every slot is busy
        (the caller waits) or the checkout could not be prepared (the result
        says why)."""
        if self.result(sha):
            return True
        busy = {r.get("slot") for _, r in self._running()}
        slot = next((k for k in range(max(1, self.o.cfg.local_ci_slots))
                     if k not in busy), None)
        if slot is None:
            if not full:
                self._pr_waiting = self.clock()
            return False
        wt = os.path.join(self.base, f"sonora-ci-{slot}")
        logdir = os.path.join(self.base, "logs", sha[:12])
        os.makedirs(logdir, exist_ok=True)
        why = self._checkout(wt, sha)
        if why:
            self._set(sha, state="fail", summary=why, excerpt=why,
                      ended=self.clock())
            self._post(sha, "failure", why)
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
                   SONORA_LOCAL_CI_DIR=logdir.replace("\\", "/"))
        kw = {}
        if sys.platform == "win32":
            kw["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                                   | subprocess.DETACHED_PROCESS)
        else:
            kw["start_new_session"] = True
        subprocess.Popen(["bash", "-c", cmd], cwd=wt, env=env,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, **kw)
        self._set(sha, state="running", slot=slot, dir=logdir,
                  started=self.clock(), full=full)
        self._post(sha, "pending", "build and tests running on the local "
                   "machine")
        return True

    def _checkout(self, wt, sha):
        root = self.o.root
        r = _git(["fetch", "-q", "origin", "main", sha], root)
        if r.returncode:
            # a local branch's commit is already in the object store
            if _git(["cat-file", "-e", sha], root).returncode:
                return f"git fetch {sha[:8]}: {r.stderr.strip()[:200]}"
            _git(["fetch", "-q", "origin", "main"], root)
        if not os.path.isdir(wt):
            os.makedirs(self.base, exist_ok=True)
            r = _git(["worktree", "add", "-f", "--detach", wt, sha], root)
            if r.returncode:
                return f"git worktree add: {r.stderr.strip()[:200]}"
        for args in (["merge", "--abort"], ["reset", "-q", "--hard"],
                     ["checkout", "-q", "-f", "--detach", sha],
                     ["clean", "-fdxq", "-e", "build-ui-container",
                      "-e", "build-container"]):
            r = _git(args, wt)
            if r.returncode and args[0] in ("checkout", "clean"):
                return f"git {args[0]}: {r.stderr.strip()[:200]}"
        # judged as it would land: merged with today's main
        r = _git(["-c", "user.name=local-ci", "-c",
                  "user.email=local-ci@localhost", "merge", "-q",
                  "--no-edit", "origin/main"], wt)
        if r.returncode:
            _git(["merge", "--abort"], wt)
            return "conflicts with main"
        return None

    def poll(self):
        """Finishes the runs whose shell wrote rc; posts their status."""
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
            self._finish(sha, rc == 0, summary, d)

    def _finish(self, sha, ok, summary, d):
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
        self._set(sha, state="pass" if ok else "fail", summary=summary[-2000:],
                  excerpt=excerpt[-4000:], ended=self.clock())
        last = [ln for ln in summary.strip().splitlines() if ln][-1:] or [""]
        self._post(sha, "success" if ok else "failure", last[0])
        self.o.store.event("local-ci", "local_ci_" + ("pass" if ok else "fail"),
                           f"{sha[:8]} {last[0][:120]}")

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
        # A PR waiting for a slot goes first; main's full run takes the
        # slot for an hour.
        if now - getattr(self, "_pr_waiting", 0) < 300:
            return
        self._main_checked = now
        if _git(["fetch", "-q", "origin", "main"], self.o.root).returncode:
            return
        sha = _git(["rev-parse", "origin/main"], self.o.root).stdout.strip()
        if sha and self.result(sha) is None:
            self.start(sha, full=True)

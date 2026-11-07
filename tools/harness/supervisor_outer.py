"""The outer watchdog: the one thing the daemon cannot do for itself is
notice that it died, hung, or runs yesterday's code. tools/supervisor.py
runs this loop as its own process next to the daemon.

  daemon alive    status.json is rewritten every tick; one older than
                  daemon_stale_s with a dead pid is restarted (the daemon
                  died when the disk filled); one with a live pid is hung:
                  the second sweep kills and restarts it
  daemon current  the daemon's checkout is compared with origin/main on the
                  harness paths; a difference is an alert (the daemon ran a
                  day-old checkout and missed auto-adopt). The restart on
                  main is the daemon's own (merge-throughput re-exec); here
                  it is named, so nobody has to notice it by hand
  disk            below the critical threshold the restart is refused and the
                  owner is told; the in-daemon check pauses the pools

Alerts go to planning/orchestrator/supervisor-alerts.json; the daemon folds
them into status.json so the dashboard shows one list.
"""
import json
import os
import subprocess
import sys
import time

from . import supervisor as sv_mod

HARNESS_PATHS = ("tools/harness", "tools/orchestrator.py",
                 "tools/build-slot.sh", "tools/supervisor.py")
FETCH_EVERY_S = 600.0


def default_spawn(root, log_path):
    """`python tools/orchestrator.py run --live`, detached, output to the
    daemon log; returns the pid."""
    kw = {}
    if sys.platform == "win32":
        kw["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                               | subprocess.DETACHED_PROCESS)
    else:
        kw["start_new_session"] = True
    out = open(log_path, "ab")
    p = subprocess.Popen([sys.executable, os.path.join(root, "tools",
                                                       "orchestrator.py"),
                          "run", "--live"], cwd=root, stdin=subprocess.DEVNULL,
                         stdout=out, stderr=subprocess.STDOUT, **kw)
    out.close()
    return p.pid


def default_git(root, *args):
    try:
        r = subprocess.run(["git", "-C", root, *args], capture_output=True,
                           text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


class Outer:
    def __init__(self, root, sys_, th=None, clock=time.time, spawn=None,
                 git=None):
        self.root = os.path.abspath(root)
        self.odir = os.path.join(self.root, "planning", "orchestrator")
        self.hdir = os.path.join(self.root, ".harness")
        self.sys = sys_
        self.clock = clock
        self.th = th or self._load_thresholds()
        self.spawn = spawn or default_spawn
        self.git = git or default_git
        self.alerts = sv_mod.Alerts(clock, self.th, self._notify)
        self.log_path = os.path.join(self.hdir, sv_mod.LOG)
        self.stale_sweeps = 0
        self.last_fetch = 0.0
        self.actions = []

    def _load_thresholds(self):
        try:
            import tomllib
            with open(os.path.join(self.odir, "orchestrator.toml"),
                      "rb") as f:
                d = tomllib.load(f).get("supervisor")
        except Exception:
            d = None
        return sv_mod.Thresholds.from_dict(d)

    # -- plumbing ----------------------------------------------------------
    def _notify(self, title, text):
        self.sys.notify(title, text)

    def log(self, what, reason, ok=True):
        self.actions.append((what, reason, ok))
        line = (time.strftime("%Y-%m-%d %H:%M:%S",
                              time.localtime(self.clock()))
                + f" outer: {what}" + ("" if ok else " (failed)")
                + f" -- {reason}\n")
        try:
            os.makedirs(self.hdir, exist_ok=True)
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write(line)
        except OSError:
            pass

    def read_status(self):
        path = os.path.join(self.odir, "status.json")
        try:
            with open(path, encoding="utf-8") as f:
                d = json.load(f)
            mtime = os.path.getmtime(path)
        except (OSError, ValueError):
            return None, None
        return (d if isinstance(d, dict) else {}), mtime

    # -- checks ------------------------------------------------------------
    def check_daemon(self, now):
        st, mtime = self.read_status()
        pid = (st or {}).get("daemon_pid")
        age = now - (st.get("updated_ts") if st and st.get("updated_ts")
                     else (mtime or 0))
        alive = self.sys.pid_alive(pid) if pid else None
        if st is not None and age <= self.th.daemon_stale_s:
            self.stale_sweeps = 0
            return
        self.stale_sweeps += 1
        where = "no status.json" if st is None else \
            f"status.json {int(age // 60)} min old"
        if alive:
            if self.stale_sweeps < 2:
                self.alert_("daemon:hung", f"daemon pid {pid} alive but "
                            f"{where}; killed next sweep", check="daemon")
                return
            self.log("kill daemon", f"pid {pid} hung: {where}")
            self.sys.kill(pid)
        self._restart(f"{where}" + (f", pid {pid} dead" if pid and not alive
                                    else ""))

    def _restart(self, why):
        free = self.sys.disk_free_gb(self.root)
        if free is not None and free < self.th.disk_critical_gb:
            self.alert_("daemon:down", f"daemon down ({why}) and disk "
                        f"critical ({free:.0f} GB): not restarted; free "
                        "disk first", check="daemon")
            return
        if not self.th.daemon_restart:
            self.alert_("daemon:down", f"daemon down ({why}); restart is off",
                        check="daemon")
            return
        try:
            pid = self.spawn(self.root, os.path.join(self.hdir,
                                                     "daemon.out.log"))
        except Exception as e:
            self.log("restart daemon", f"{why}: spawn failed {e!r}", ok=False)
            self.alert_("daemon:down", f"daemon down ({why}); restart "
                        f"failed: {e!r}", check="daemon")
            return
        self.log("restart daemon", f"{why}; new pid {pid}")
        self.alert_("daemon:restarted", f"daemon restarted (pid {pid}): "
                    f"{why}", check="daemon")
        self.stale_sweeps = 0

    def check_current(self, now):
        if now - self.last_fetch >= FETCH_EVERY_S:
            self.last_fetch = now
            if self.git(self.root, "fetch", "-q", "origin", "main") is None:
                self.alert_("daemon:fetch", "git fetch origin main failed in "
                            "the daemon checkout", level="warn",
                            check="daemon")
                return
        out = self.git(self.root, "diff", "--name-only", "origin/main", "--",
                       *HARNESS_PATHS)
        if out is None:
            return
        files = [x for x in out.splitlines() if x.strip()]
        if files:
            self.alert_("daemon:stale-code", f"daemon checkout differs from "
                        f"origin/main on {len(files)} harness file(s): "
                        + ", ".join(files[:4]) + ("..." if len(files) > 4
                                                   else ""),
                        check="daemon")

    def check_disk(self, now):
        free = self.sys.disk_free_gb(self.root)
        if free is not None and free < self.th.disk_critical_gb:
            self.alert_("disk:critical", f"disk critical: {free:.0f} GB free",
                        check="disk")

    def alert_(self, id, text, level="alert", check=""):
        a = self.alerts.raise_(id, text, level, check)
        if a.count == 1:
            self.log(f"alert {id}", text)
        return a

    # -- the sweep -----------------------------------------------------------
    def tick(self, now=None):
        now = self.clock() if now is None else now
        self.alerts.begin()
        for check in (self.check_daemon, self.check_current, self.check_disk):
            try:
                check(now)
            except Exception as e:  # one broken check must not stop the rest
                self.log(check.__name__, repr(e)[:300], ok=False)
        self.alerts.end()
        rows = self.alerts.rows()
        try:
            os.makedirs(self.odir, exist_ok=True)
            sv_mod.write_outer_alerts(self.odir, rows)
        except OSError as e:
            self.log("write alerts", repr(e), ok=False)
        return rows

    def run(self, interval_s=None, once=False, sleep=time.sleep):
        every = interval_s or self.th.interval_s
        while True:
            rows = self.tick()
            if once:
                return rows
            sleep(every)

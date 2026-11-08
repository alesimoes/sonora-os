"""The supervisor: a plain watchdog inside the daemon's tick (owner,
2026-11-07: "we have become the orchestrators; nothing monitors the
agents"). Every few minutes it runs the checks in supervisor_checks.py; each
heals what it can (wake, kill, reap, adopt, janitor, pause) and raises an
alert for what it cannot. Every action is logged with its reason, to
.harness/supervisor.log and as a `supervisor` event; the alerts go into
status.json (`alerts`) for the dashboard, and the owner gets one desktop
notification per alert, repeated at `notify_repeat_s` while it stands.

Thresholds come from the [supervisor] table of orchestrator.toml (see
Thresholds for the keys and defaults). The daemon itself is watched from
outside by tools/supervisor.py, which shares this module's alert file.
"""
import json
import os
import time
from dataclasses import dataclass, field, fields

from .status import write_atomic

LOG = "supervisor.log"
OUTER_ALERTS = "supervisor-alerts.json"   # written by tools/supervisor.py


@dataclass
class Thresholds:
    # seconds between two sweeps (the tick is 30 s; checks call gh and git)
    interval_s: float = 180.0
    # a suspect run (no beat for cfg.suspect_s) is nudged once
    nudge: bool = True
    # lock: a waiter or holder older than this is an alert
    lock_wait_alert_s: float = 2 * 3600.0
    lock_hold_alert_s: float = 3 * 3600.0
    # an orphan (dead pid, finished run, no gate running) is removed at once
    lock_reap: bool = True
    # a task that waits for a fixer or integrator with no run for this long
    unowned_max_s: float = 30 * 60.0
    # disk, in GB free on the drive of the checkout
    disk_low_gb: float = 40.0
    disk_critical_gb: float = 15.0
    disk_janitor_every_s: float = 3600.0
    # a queued head whose PR is not open for this long is an alert
    queue_orphan_s: float = 15 * 60.0
    # an open PR the daemon's own sweep (train.LIST_TTL_S cadence) has not
    # adopted after this long is adopted here and, if that fails, alerted
    adopt_grace_s: float = 600.0
    # a task, wake or feature with no run and no progress for this long is
    # an alert (PROCESS.md: "stalled more than 2 h"); an open PR not merged
    # for this long too (harness/stall_check.py)
    stall_alert_s: float = 2 * 3600.0
    pr_stall_s: float = 3 * 3600.0
    # owner notifications
    notify: bool = True
    notify_repeat_s: float = 3600.0
    # the outer watchdog (tools/supervisor.py)
    daemon_stale_s: float = 300.0
    daemon_restart: bool = True

    @classmethod
    def from_dict(cls, d):
        d = d or {}
        known = {f.name: f.type for f in fields(cls)}
        kw = {}
        for k, v in d.items():
            if k in known:
                kw[k] = bool(v) if known[k] is bool else float(v)
        return cls(**kw)


@dataclass
class Alert:
    id: str
    level: str          # "warn" | "alert"
    text: str
    since: float
    last: float
    count: int = 1
    check: str = ""
    notified: float = 0.0

    def row(self):
        return {"id": self.id, "level": self.level, "text": self.text,
                "since": self.since, "last": self.last, "count": self.count,
                "check": self.check}


class Alerts:
    """Alerts raised this sweep stand; one not raised again is cleared.
    Notification is per id, repeated while it stands."""

    def __init__(self, clock, th, notify):
        self.clock, self.th, self._notify = clock, th, notify
        self.cur = {}
        self._seen = set()

    def begin(self):
        self._seen = set()

    def raise_(self, id, text, level="alert", check=""):
        now = self.clock()
        a = self.cur.get(id)
        if a is None:
            a = Alert(id, level, text, now, now, check=check)
            self.cur[id] = a
        else:
            a.text, a.level, a.last, a.count = text, level, now, a.count + 1
        self._seen.add(id)
        if level == "alert" and self.th.notify and \
                now - a.notified >= self.th.notify_repeat_s:
            a.notified = now
            self._notify(f"SonoraOS harness: {check or id}", text)
        return a

    def end(self):
        for k in [k for k in self.cur if k not in self._seen]:
            del self.cur[k]

    def rows(self):
        return [a.row() for a in sorted(self.cur.values(),
                                        key=lambda a: a.since)]


@dataclass
class Action:
    ts: float
    check: str
    what: str
    reason: str
    ok: bool = True


class Supervisor:
    """Runs inside the daemon: `sv.tick(now)` from Orchestrator.tick()."""

    def __init__(self, orch, sys_, th=None, clock=None, checks=None):
        from . import stall_check, supervisor_checks
        self.o = orch
        self.sys = sys_
        self.clock = clock or orch.clock
        self.th = th or Thresholds.from_dict(getattr(orch.cfg, "supervisor",
                                                     None))
        self.alerts = Alerts(self.clock, self.th, self._notify)
        self.checks = checks if checks is not None else \
            supervisor_checks.DAEMON_CHECKS + (stall_check.check_stalls,)
        self.actions = []
        self.last_sweep = 0.0
        self.mem = {}            # per-check memory (first seen, nudged...)
        self.log_path = os.path.join(orch.hdir, LOG)

    # -- plumbing ----------------------------------------------------------
    def _notify(self, title, text):
        ok = self.sys.notify(title, text)
        self._log("notify", "sent" if ok else "failed", f"{title}: {text}")

    def _log(self, check, what, reason, ok=True):
        a = Action(self.clock(), check, what, reason, ok)
        self.actions.append(a)
        del self.actions[:-500]
        line = (time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(a.ts))
                + f" {check}: {what}" + ("" if ok else " (failed)")
                + f" -- {reason}\n")
        try:
            os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write(line)
        except OSError:
            pass
        try:
            self.o.store.event("supervisor", f"{check}:{what}", reason[:300])
        except Exception:  # the log line stands on its own
            pass
        return a

    def act(self, check, what, reason, fn=None):
        """Perform fn (if any) and log it with its reason."""
        ok = True
        if fn is not None:
            try:
                ok = fn() is not False
            except Exception as e:  # one failed action is logged, not fatal
                ok, reason = False, f"{reason}: {e!r}"
        return self._log(check, what, reason, ok)

    def alert(self, id, text, level="alert", check=""):
        return self.alerts.raise_(id, text, level, check)

    def remember(self, check, key, default=None):
        return self.mem.setdefault(check, {}).setdefault(key, default)

    def forget(self, check, key):
        self.mem.get(check, {}).pop(key, None)

    # -- the sweep -----------------------------------------------------------
    def due(self, now):
        return now - self.last_sweep >= self.th.interval_s

    def tick(self, now, force=False):
        if not force and not self.due(now):
            return False
        self.last_sweep = now
        self.alerts.begin()
        for check in self.checks:
            try:
                check(self, now)
            except Exception as e:  # one broken check must not stop the rest
                self._log(getattr(check, "__name__", "check"), "crashed",
                          repr(e)[:300], ok=False)
        self._outer_alerts()
        self.alerts.end()
        return True

    def _outer_alerts(self):
        """tools/supervisor.py (the process that watches the daemon) leaves
        its alerts in a file; they show on the same board."""
        path = os.path.join(self.o.odir, OUTER_ALERTS)
        try:
            with open(path, encoding="utf-8") as f:
                rows = json.load(f)
        except (OSError, ValueError):
            return
        if not isinstance(rows, list):
            return
        for r in rows:
            if isinstance(r, dict) and r.get("id") and r.get("text"):
                self.alert("outer:" + r["id"], r["text"],
                           r.get("level", "alert"), r.get("check", "daemon"))

    def status(self):
        return {"alerts": self.alerts.rows(),
                "supervisor": {"last_sweep": self.last_sweep or None,
                               "interval_s": self.th.interval_s,
                               "actions": [a.__dict__ for a in
                                           self.actions[-20:]]}}


def write_outer_alerts(odir, rows):
    write_atomic(os.path.join(odir, OUTER_ALERTS), rows)

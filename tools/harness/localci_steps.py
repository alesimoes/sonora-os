"""Per-step results of the local gate (owner, 2026-11-06).

tools/local-ci.sh writes `<step>.status` in the run's log directory after
each step: one line `state=pass|fail|running seconds=N sha=HEAD`. This module
reads them and posts each as the commit status `local-ci/<step>`, next to the
aggregate `local-ci` (which stays the one required check and still turns
green only when passed() says every step did). No docker, no git: files in,
statuses out, so it is testable with fakes.
"""
import glob
import json
import os

STATE_TO_FORGE = {"running": "pending", "pass": "success", "fail": "failure"}


def parse_status(text):
    """{'state':..., 'seconds': int, 'sha': ...} or None for junk (a half
    written file, an unknown state)."""
    fields = dict(p.split("=", 1) for p in text.split() if "=" in p)
    if fields.get("state") not in STATE_TO_FORGE:
        return None
    try:
        seconds = int(fields.get("seconds", "0"))
    except ValueError:
        return None
    return {"state": fields["state"], "seconds": seconds,
            "sha": fields.get("sha", "")}


def read_steps(logdir):
    """{step: parsed status} for every readable <step>.status in logdir."""
    out = {}
    for path in glob.glob(os.path.join(logdir, "*.status")):
        step = os.path.basename(path)[:-len(".status")]
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                st = parse_status(f.read())
        except OSError:
            continue
        if st:
            out[step] = st
    return out


def clear_steps(logdir):
    """A new run in a reused log directory must not report the last one."""
    for path in glob.glob(os.path.join(logdir, "*.status*")):
        try:
            os.remove(path)
        except OSError:
            pass


def describe(st):
    if st["state"] == "running":
        return "running"
    return "%s in %d s" % ("passed" if st["state"] == "pass" else "failed",
                           st["seconds"])


class StepPoster:
    """Posts `local-ci/<step>` once per change of state. What was posted is
    kept in the store (meta `localci-steps:<key>`), so a daemon restart does
    not repost, and a step posted pending is posted again when it ends."""

    def __init__(self, store, forge):
        self.store, self.forge = store, forge

    def _posted(self, key):
        try:
            return json.loads(self.store.get_meta("localci-steps:" + key)
                              or "{}")
        except ValueError:
            return {}

    def sync(self, key, heads, logdir):
        """Posts the steps whose state changed to every head of the run;
        returns the number of steps posted."""
        posted = self._posted(key)
        n = 0
        for step, st in sorted(read_steps(logdir).items()):
            if posted.get(step) == st["state"]:
                continue
            for h in heads:
                try:
                    self.forge.set_status(h, STATE_TO_FORGE[st["state"]],
                                          "local-ci/" + step, describe(st))
                except Exception as e:   # the aggregate stands without it
                    self.store.event("local-ci", "step_status_failed",
                                     str(e)[:200])
            posted[step] = st["state"]
            n += 1
        if n:
            self.store.set_meta("localci-steps:" + key, json.dumps(posted))
        return n

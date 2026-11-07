"""Per-step results of the local gate (from #464, owner 2026-11-06).

tools/local-ci.sh writes `<step>.status` in the run's log directory as each
step starts and ends: one line `state=running|pass|fail seconds=N wait=W
sha=HEAD`, W being the seconds the step waited for the machine lock (kept
apart from its own time, 2026-11-07). This module only reads them: the
dashboard's builder card shows the step running and what each one cost. The
`local-ci` status stays the one verdict (localci.passed); a step file never
decides anything. No docker, no git: files in, dicts out.
"""
import glob
import os

STATES = ("running", "pass", "fail")
RUN_FILES = ("rc", "summary", "flakes", "lock.wait")


def parse_status(text):
    """{'state', 'seconds', 'wait', 'sha'} or None for junk (a half-written
    file, an unknown state)."""
    fields = dict(p.split("=", 1) for p in text.split() if "=" in p)
    if fields.get("state") not in STATES:
        return None
    try:
        seconds = int(fields.get("seconds", "0"))
        wait = int(fields.get("wait", "0"))
    except ValueError:
        return None
    return {"state": fields["state"], "seconds": seconds, "wait": wait,
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


def current_step(logdir):
    """The step a run is on: the one whose status says running."""
    return next((s for s, st in sorted(read_steps(logdir).items())
                 if st["state"] == "running"), None)


def clear_run(logdir):
    """A new run in a reused log directory must not report the last one:
    its verdict files, step statuses, first logs of rerun steps, flakes."""
    paths = [os.path.join(logdir, f) for f in RUN_FILES]
    for pat in ("*.status", "*.first.log", "*.lockwait"):
        paths += glob.glob(os.path.join(logdir, pat))
    for p in paths:
        try:
            os.remove(p)
        except OSError:
            pass

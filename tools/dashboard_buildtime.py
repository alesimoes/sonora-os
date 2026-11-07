"""Build time: the running lot's steps with durations, an estimate from the
local CI summaries of past runs, and a rough start for the next lot."""
import os
import re
import time

from dashboard_util import orch_root

STEP_RE = re.compile(r"^(ok|FAIL)\s+(\S+)\s+\((\d+) s\)")


def parse_summary(path):
    """(planned steps, {step: (status, seconds)}) from a local-ci summary file."""
    planned, done = [], {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for i, ln in enumerate(f):
                ln = ln.strip()
                if i == 0 and ln.startswith("local-ci: ") and not re.match(r"local-ci: (failed|nothing|stopped)", ln):
                    planned = ln.split()[1:]
                m = STEP_RE.match(ln)
                if m:
                    done[m.group(2)] = (m.group(1), int(m.group(3)))
    except OSError:
        pass
    return planned, done


def build_time(orch):
    """The running lot's steps with durations, an estimate from past runs'
    summaries, and a rough start for the next lot. All epochs; the page
    formats them in local time."""
    b = (orch or {}).get("builder") or {}
    run, last = b.get("running"), b.get("last")
    dirs = {os.path.join(orch_root(), ".harness", "ci", "logs")}
    for h in (run, last):
        if h and h.get("dir"):
            dirs.add(os.path.dirname(h["dir"].rstrip("\\/")))
    hist, totals = {}, []
    for base in dirs:
        try:
            names = os.listdir(base)
        except OSError:
            continue
        for n in names:
            planned, done = parse_summary(os.path.join(base, n, "summary"))
            ok = [(k, v[1]) for k, v in done.items() if v[0] == "ok"]
            for k, sec in ok:
                hist.setdefault(k, []).append(sec)
            if planned and len(ok) == len(planned):
                totals.append(sum(sec for _, sec in ok))
    avg = {k: {"avg": sorted(v)[len(v) // 2], "n": len(v)} for k, v in hist.items()}  # median: one cold-cache run must not skew it
    out = {"avg": avg, "lot": None, "next": None}
    now = time.time()
    if not run or not run.get("dir"):
        return out
    planned, done = parse_summary(os.path.join(run["dir"], "summary"))
    if not planned:
        return out
    started = run.get("started") or now
    try:
        step_start = os.path.getmtime(os.path.join(run["dir"], "summary")) if done else started
    except OSError:
        step_start = started
    steps, current_seen, remaining, missing, est_total = [], False, 0.0, [], 0.0
    for name in planned:
        a = avg.get(name)
        if name in done:
            st, sec = done[name]
            steps.append({"name": name, "status": "feito" if st == "ok" else "falhou", "dur": sec, "avg": a and a["avg"], "n": a["n"] if a else 0})
            est_total += sec
            continue
        if not current_seen:
            current_seen = True
            el = max(0, now - step_start)
            steps.append({"name": name, "status": "rodando", "dur": el, "avg": a and a["avg"], "n": a["n"] if a else 0})
            if a:
                remaining += max(a["avg"] - el, 0)
                est_total += max(a["avg"], el)
            else:
                missing.append(name)
        else:
            steps.append({"name": name, "status": "a fazer", "dur": None, "avg": a and a["avg"], "n": a["n"] if a else 0})
            if a:
                remaining += a["avg"]
                est_total += a["avg"]
            else:
                missing.append(name)
    elapsed = now - started
    eta = None if missing else now + remaining
    out["lot"] = {"dir": run["dir"], "prs": [x for x in run.get("prs") or [] if x], "started": started, "elapsed": elapsed,
                  "steps": steps, "eta": eta, "missing": missing,
                  "progress": (min(1.0, (est_total - remaining) / est_total) if est_total else None) if not missing else None}
    q = b.get("queue") or {}
    if q.get("open_prs") or q.get("batch"):
        lot_len = sum(totals) / len(totals) if totals else None
        out["next"] = {"prs": q.get("open_prs") or [], "start": eta, "end": eta + lot_len if eta and lot_len else None}
    return out

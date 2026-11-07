"""What the dashboard reads from the orchestrator: its status.json and
state.json, the event log, a run's stream log, the local CI summaries (build
time and estimate), the coordinator's note and main's progress catalogue."""
import json
import os
import re
import time

from dashboard_sources import ROOT, orch_root, run


def read_orchestrator():
    """planning/orchestrator/status.json when the coordinator writes it; None otherwise."""
    try:
        path = os.environ.get("SONORA_ORCH_STATUS") or os.path.join(ROOT, "planning", "orchestrator", "status.json")
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def pr_of(task):
    m = re.match(r"pr-(\d+)$", str(task or ""))
    return int(m.group(1)) if m else None


def clean_orch(o, open_prs):
    """Drop what is no longer current: closed PRs, PR-less queue entries and
    runs that stopped giving signs of life. open_prs is None when GitHub
    could not be read, and then no PR is judged closed."""
    if not isinstance(o, dict):
        return o
    o = dict(o)
    now = time.time()
    if open_prs is not None:
        def gone(item):
            n = pr_of(item.get("task")) if isinstance(item, dict) else None
            return n is not None and n not in open_prs
        o["train"] = [r for r in o.get("train") or [] if r.get("state") == "merged" or r.get("pr") in open_prs]
        o["blocked"] = [b for b in o.get("blocked") or [] if not gone(b)]
        o["waits_on_owner"] = [b for b in o.get("waits_on_owner") or [] if not gone(b)]
        o["features"] = [f for f in o.get("features") or []
                         if not (f.get("pr") and f["pr"] not in open_prs and f.get("state") != "merged")]
        b = dict(o.get("builder") or {})
        q = dict(b.get("queue") or {})
        seen = []
        for n in q.get("prs") or []:
            if n in open_prs and n not in seen:
                seen.append(n)
        q["open_prs"] = seen
        b["queue"] = q
        o["builder"] = b
    o["runs"] = [r for r in o.get("runs") or [] if now - (r.get("last_beat") or r.get("started") or now) < 1800]
    return o


def read_coordinator():
    """What the coordinator session (Claude in the terminal) says it is doing:
    <orchestrator root>/.harness/coordinator.json, written by the session."""
    try:
        with open(os.path.join(orch_root(), ".harness", "coordinator.json"), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def read_progress():
    """How much of the project is done, from main's catalogue
    (planning/progress.json on origin/main, tallied by tools/progress.py):
    only merged and verified work counts, as in planning/PROGRESS.md."""
    import importlib.util
    root = orch_root()
    run(["git", "-C", root, "fetch", "-q", "origin", "main"], timeout=60)
    raw = run(["git", "-C", root, "show", "origin/main:planning/progress.json"])
    spec = importlib.util.spec_from_file_location(
        "sonora_progress", os.path.join(os.path.dirname(os.path.abspath(__file__)), "progress.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    overall, fronts, _ = mod.tally(json.loads(raw))
    one = lambda t: {"pct": t.pct(), "done": t.done, "total": t.total, "doing": t.doing}
    return dict(one(overall), fronts=[dict(one(t), title=f["title"]) for f, t, _ in fronts])


def orch_state():
    try:
        with open(os.path.join(orch_root(), "planning", "orchestrator", "state.json"), encoding="utf-8") as f:
            s = json.load(f)
    except Exception:
        return {}
    for k in ("tasks", "features", "runs"):
        v = s.get(k) or []
        s[k] = list(v.values()) if isinstance(v, dict) else v
    return s


def tail_lines(path, max_bytes=600000):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - max_bytes))
            return f.read().decode("utf-8", "replace").splitlines()[1:]
    except OSError:
        return []


def events_of(ids, n=20):
    out = []
    for ln in tail_lines(os.path.join(orch_root(), "planning", "orchestrator", "events.jsonl"), 1500000):
        try:
            e = json.loads(ln)
        except ValueError:
            continue
        if e.get("task_id") in ids:
            out.append({"ts": e.get("ts"), "kind": e.get("kind"), "detail": (e.get("detail") or "")[:300]})
    return out[-n:]


def tool_summary(name, inp):
    inp = inp or {}
    for k in ("description", "command", "file_path", "pattern", "path", "url", "prompt"):
        if inp.get(k):
            v = str(inp[k])
            if k == "description" and inp.get("command"):
                v += ": " + str(inp["command"])
            return v[:240]
    return json.dumps(inp)[:200]


def run_activity(stream_path, n=25):
    """What a run did last, from its claude stream-json log."""
    items = []
    for ln in tail_lines(stream_path):
        try:
            d = json.loads(ln)
        except ValueError:
            continue
        t = d.get("type")
        if t == "assistant":
            for c in (d.get("message") or {}).get("content") or []:
                if c.get("type") == "text" and c.get("text", "").strip():
                    items.append({"kind": "texto", "text": c["text"].strip()[:600]})
                elif c.get("type") == "tool_use":
                    items.append({"kind": c.get("name") or "tool", "text": tool_summary(c.get("name"), c.get("input"))})
        elif t == "user":
            for c in (d.get("message") or {}).get("content") or []:
                if isinstance(c, dict) and c.get("type") == "tool_result" and c.get("is_error"):
                    body = c.get("content")
                    if isinstance(body, list):
                        body = " ".join(x.get("text", "") for x in body if isinstance(x, dict))
                    items.append({"kind": "erro", "text": str(body)[:300]})
        elif t == "result":
            items.append({"kind": "fim", "text": (d.get("result") or d.get("subtype") or "")[:600]})
    return items[-n:]


def task_view(t):
    if not t:
        return None
    files = t.get("files") or []
    return {"id": t.get("id"), "state": t.get("state"), "agent": t.get("agent"), "track": t.get("track"),
            "kind": t.get("kind"), "branch": t.get("branch"), "pr": t.get("pr"),
            "brief": (t.get("brief") or "")[:1500], "note": (t.get("note") or "")[:2000],
            "attempts": t.get("attempts"), "fix_kind": t.get("fix_kind"),
            "files": files[:12], "nfiles": len(files)}


def builder_log(d):
    """The tail of the newest step log of the batch that is running."""
    try:
        logs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".log")]
        with open(max(logs, key=os.path.getmtime), encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-25:])[-3000:]
    except (OSError, ValueError, TypeError):
        return ""


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

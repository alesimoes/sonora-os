#!/usr/bin/env python3
"""Local live board: agents, open PRs, merge watchers, containers.

    python tools/dashboard.py [--port 8765] [--agents FILE]

Listens on 127.0.0.1 only. Open http://127.0.0.1:8765/ . Read-only: it runs
gh, docker, git and ps, and never changes anything.

Agent state is inferred, not reported by the agent: a container that mounts the
agent's worktree means "building/testing"; a file or git change in the last
few minutes means "working"; neither means "waiting". The labels come from the
JSON file given with --agents ({"<agent id>": {"label": ..., "track": ...,
"status": "running|done|paused", "pr": N, "worktree": "<id>"}}), which the coordinator keeps up to date.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WORKING_SECONDS = 180
state = {"usage": {"sessions": [], "week": None, "five_hour": None}, "updated": 0, "agents": [], "prs": [], "watchers": [], "containers": [], "errors": [], "orchestrator": None, "merged": [], "lock": []}
lock = threading.Lock()


def run(cmd, timeout=40):
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        return (r.stdout or "") if r.returncode == 0 else ""
    except Exception:
        return ""


def norm(path):
    return path.replace("\\", "/").rstrip("/").lower()


STEP_CACHE = {}


def failing_step(job):
    # A finished job never changes, so one lookup per job id is enough.
    if job not in STEP_CACHE:
        try:
            j = json.loads(run(["gh", "api", "repos/{owner}/{repo}/actions/jobs/%s" % job]) or "{}")
        except Exception:
            j = {}
        if not j:
            return None
        STEP_CACHE[job] = next((st.get("name") for st in j.get("steps", []) if st.get("conclusion") == "failure"), "") or ""
    return STEP_CACHE[job]


def read_prs():
    out = run(["gh", "pr", "list", "--state", "open", "--limit", "60", "--json",
               "number,title,headRefName,baseRefName,mergeStateStatus,mergeable,createdAt,isDraft,statusCheckRollup"])
    prs = []
    for p in json.loads(out or "[]"):
        failed, pending, passed, cancelled, details = [], [], 0, [], []
        for c in p.get("statusCheckRollup") or []:
            name = c.get("name") or c.get("context") or "?"
            if c.get("status") not in (None, "COMPLETED"):
                pending.append(name)
            elif c.get("conclusion") in ("FAILURE", "CANCELLED", "TIMED_OUT", "STARTUP_FAILURE"):
                failed.append(name)
                if c.get("conclusion") == "CANCELLED":
                    cancelled.append(name)
                m = re.search(r"/job/(\d+)", c.get("detailsUrl") or "")
                details.append({"name": name, "conclusion": c.get("conclusion"),
                                "step": failing_step(m.group(1)) if m and c.get("conclusion") != "CANCELLED" else None})
            elif c.get("conclusion") in ("SUCCESS", "SKIPPED", "NEUTRAL"):
                passed += 1
        prs.append({"number": p["number"], "title": p["title"], "branch": p["headRefName"],
                    "merge": p.get("mergeStateStatus"), "created": p["createdAt"],
                    "failed": failed, "pending": pending, "passed": passed,
                    "base": p.get("baseRefName"), "mergeable": p.get("mergeable"),
                    "cancelled": cancelled, "failed_details": details,
                    "total": passed + len(failed) + len(pending)})
    return sorted(prs, key=lambda p: p["number"])


def read_merged():
    # "Today" is the local day: the UTC date rolls over in the evening.
    t = time.localtime()
    start = time.mktime((t.tm_year, t.tm_mon, t.tm_mday, 0, 0, 0, 0, 0, -1))
    since = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(start))
    out = run(["gh", "pr", "list", "--state", "merged", "--limit", "100", "--search", "merged:>=" + since,
               "--json", "number,title,mergedAt"])
    rows = [p for p in json.loads(out or "[]") if p["mergedAt"] >= since]
    return sorted(rows, key=lambda p: p["mergedAt"], reverse=True)


def read_watchers():
    rows = []
    for line in run(["ps", "-ef"]).splitlines():
        m = re.search(r"(?:^|[\s/])sh\s+(?:\./)?tools/merge-when-green\.sh\s+(\d+)", line)
        if m:
            parts = line.split()
            rows.append({"pr": int(m.group(1)), "pid": parts[1], "since": parts[4]})
    seen, uniq = set(), []
    for r in rows:
        if (r["pr"], r["pid"]) not in seen:
            seen.add((r["pr"], r["pid"]))
            uniq.append(r)
    return sorted(uniq, key=lambda r: r["pr"])


def read_containers():
    ids = run(["docker", "ps", "-q"]).split()
    if not ids:
        return []
    fmt = "{{.Id}}|{{.Config.Image}}|{{.State.StartedAt}}|{{range .Mounts}}{{.Source}};{{end}}"
    rows = []
    for line in run(["docker", "inspect", "--format", fmt] + ids).splitlines():
        cid, image, started, mounts = (line.split("|") + ["", "", "", ""])[:4]
        rows.append({"id": cid[:12], "image": image, "started": started,
                     "mounts": [norm(m) for m in mounts.split(";") if m]})
    return rows


def worktree_activity(path):
    """Most recent sign of life: modified files or git metadata, as epoch seconds."""
    newest, dirty = 0.0, 0
    status = run(["git", "-C", path, "status", "--porcelain", "--untracked-files=no"], timeout=20)
    for line in status.splitlines():
        dirty += 1
        f = os.path.join(path, line[3:].strip().strip('"').split(" -> ")[-1])
        try:
            newest = max(newest, os.path.getmtime(f))
        except OSError:
            pass
    gitfile = os.path.join(path, ".git")
    try:
        if os.path.isfile(gitfile):
            gitdir = open(gitfile).read().split(":", 1)[1].strip()
            for n in ("HEAD", "index", "logs/HEAD"):
                try:
                    newest = max(newest, os.path.getmtime(os.path.join(gitdir, n)))
                except OSError:
                    pass
    except Exception:
        pass
    return newest, dirty


def stamp_done(labels, agents_file):
    """When an agent first shows as done, remember the time next to the agents
    file. The first run seeds every already-done agent with 0 (earlier days),
    so only real transitions count as "done today"."""
    if not agents_file:
        return labels
    side = agents_file + ".done.json"
    try:
        seen = json.load(open(side, encoding="utf-8"))
    except (OSError, ValueError):
        seen = {k: 0 for k, m in labels.items() if m.get("status") == "done"}
        json.dump(seen, open(side, "w", encoding="utf-8"))
    changed = False
    for k, m in labels.items():
        if m.get("status") == "done" and k not in seen:
            seen[k] = int(time.time())
            changed = True
        if k in seen and not m.get("done_at") and seen[k]:
            m["done_at"] = seen[k]
    if changed:
        json.dump(seen, open(side, "w", encoding="utf-8"))
    return labels


def read_agents(labels, containers, lock):
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=8) as ex:
        return list(ex.map(lambda kv: one_agent(kv[0], kv[1], containers, lock), labels.items()))


def one_agent(aid, meta, containers, lock):
    """One coordinator-session agent. The JSON file is the truth for what it
    does and whether it is finished; the machine lock (build-slot.sh --list)
    says whether it is using the machine or queued for it, which no file
    activity can tell."""
    d = meta.get("dir") or ("agent-" + meta.get("worktree", aid))
    path = os.path.join(ROOT, ".claude", "worktrees", d)
    frag = "/worktrees/" + d.lower()
    row = {"id": aid, "label": meta.get("label", aid), "track": meta.get("track", ""),
           "declared": meta.get("status", "running"), "pr": meta.get("pr"),
           "model": meta.get("model"), "agent": meta.get("agent"), "doing": meta.get("doing"),
           "branch": meta.get("branch"), "since": meta.get("since"), "done_at": meta.get("done_at"),
           "containers": [], "dirty": 0, "idle_seconds": None, "state": "unknown", "lock": None}
    mine = [c for c in containers if any(norm(path) in m or m.startswith(norm(path)) for m in c["mounts"])]
    row["containers"] = [c["id"] for c in mine]
    if row["declared"] in ("done", "paused"):
        row["state"] = row["declared"]
        return row
    held = next((r for r in lock if frag in r["cwd"].lower()), None)
    if held:
        row["lock"] = {"role": held["role"], "prio": held["prio"], "start": held["start"], "cmd": held["cmd"][:120]}
    if os.path.isdir(path):
        if not row["since"]:
            row["since"] = int(os.path.getctime(path))
            row["since_src"] = "worktree"
        newest, row["dirty"] = worktree_activity(path)
        if newest:
            row["idle_seconds"] = int(time.time() - newest)
    if held and held["role"] == "holds":
        row["state"] = "building"
    elif held:
        row["state"] = "waiting_lock"
    elif mine:
        row["state"] = "building"
    elif row["declared"] in ("working", "running"):
        row["state"] = "working"
    else:
        row["state"] = "waiting"
    return row


def read_usage():
    """Snapshots written by the status line script, one file per session."""
    d = os.path.join(os.path.expanduser("~"), ".claude", "dashboard-usage")
    rows = []
    try:
        for n in os.listdir(d):
            if n.endswith(".json"):
                try:
                    rows.append(json.load(open(os.path.join(d, n), encoding="utf-8")))
                except Exception:
                    pass
    except OSError:
        return {"sessions": [], "week": None, "five_hour": None}
    now = time.time()
    rows = [r for r in rows if now - r.get("ts", 0) < 6 * 3600]
    rows.sort(key=lambda r: r.get("ts", 0), reverse=True)
    pick = lambda k: next((r[k] for r in rows if r.get(k)), None)
    return {"sessions": rows, "week": pick("seven_day"), "five_hour": pick("five_hour")}


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


def refresh_loop(agents_file):
    last_pr, prs, merged, gh_ok = 0, [], [], False
    while True:
        errors = []
        try:
            if time.time() - last_pr > 20:
                prs = read_prs()
                merged = read_merged()
                last_pr = time.time()
                gh_ok = True
        except Exception as e:
            errors.append("gh: %s" % e)
        try:
            labels = json.load(open(agents_file, encoding="utf-8")) if agents_file and os.path.exists(agents_file) else {}
        except Exception as e:
            labels = {}
            errors.append("agents file: %s" % e)
        stamp_done(labels, agents_file)
        containers = read_containers()
        watchers = read_watchers()
        lk = read_lock(labels)
        orch = clean_orch(read_orchestrator(), {p["number"] for p in prs} if gh_ok else None)
        with lock:
            state.update(updated=int(time.time()), prs=prs, watchers=watchers, containers=containers, merged=merged, usage=read_usage(), orchestrator=orch, coordinator=read_coordinator(), lock=lk)
        agents = read_agents(labels, containers, lk)
        with lock:
            state.update(updated=int(time.time()), agents=agents, prs=prs, merged=merged,
                         watchers=watchers, containers=containers, errors=errors)
        time.sleep(4)


PAGE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")


# --- details for a clicked card ------------------------------------------
def orch_root():
    path = os.environ.get("SONORA_ORCH_STATUS") or os.path.join(ROOT, "planning", "orchestrator", "status.json")
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(path))))


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


LOCK_RE = re.compile(r"^(holds|waits)\s+pid=(\d+)\s+kind=(\S+)\s+user=(\S+)\s+prio=(\d+)\s+start=(\d+)\s+cwd=(\S*)\s+cmd=(.*)$")
_lock_cache = {"t": 0.0, "rows": []}


def read_lock(labels):
    """Parsed `build-slot.sh --list`, cached a few seconds: the holder first,
    then the waiters in queue order, each with who owns it."""
    if time.time() - _lock_cache["t"] > 6:
        rows = []
        for ln in heavy_lock_list():
            m = LOCK_RE.match(ln.strip())
            if m:
                rows.append({"role": m.group(1), "pid": m.group(2), "prio": int(m.group(5)), "start": int(m.group(6)),
                             "cwd": m.group(7), "cmd": m.group(8).strip()})
        _lock_cache.update(t=time.time(), rows=rows)
    rows = _lock_cache["rows"]
    for r in rows:
        low = r["cwd"].lower()
        who = None
        for aid, meta in labels.items():
            d = (meta.get("dir") or ("agent-" + meta.get("worktree", aid))).lower()
            if meta.get("status") not in ("done", "paused") and "/worktrees/" + d in low:
                who = meta.get("label", aid)
        if not who and "/.harness/ci" in low:
            who = "Builder do orquestrador (CI local)"
        if not who:
            who = os.path.basename(r["cwd"].rstrip("/")) or "?"
        r["who"] = who
        r["builder"] = "/.harness/ci" in low
    return rows


def _find_slot_script():
    # The lock script lives with the harness branch, which may not be the one
    # this checkout is on: use the first worktree that has it.
    for base in (ROOT, os.path.join(ROOT, ".claude", "worktrees", "harness-pr"), orch_root()):
        f = os.path.join(base, "tools", "build-slot.sh")
        if os.path.exists(f):
            return f
    return os.path.join(ROOT, "tools", "build-slot.sh")


GIT_BASH = next((b for b in (r"C:\Program Files\Gitinash.exe", shutil.which("bash")) if b and os.path.exists(b)), None)
SLOT_SCRIPT = _find_slot_script()


def heavy_lock_list():
    """Who holds the machine-wide heavy lock and who waits (tools/build-slot.sh)."""
    try:
        out = subprocess.run([GIT_BASH or "bash", SLOT_SCRIPT, "--list"],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=10).stdout
        return out.strip().splitlines()[:20]
    except (OSError, subprocess.SubprocessError):
        return []


def detail(kind, ident):
    s = orch_state()
    tasks, feats, runs = s.get("tasks", []), s.get("features", []), s.get("runs", [])
    if kind == "coord":
        c = read_coordinator() or {}
        return {"kind": "coord", "title": "Coordenador (Claude, sessão no terminal)", "coord": c}
    if kind == "agent":
        with lock:
            a = next((x for x in state["agents"] if x["id"] == ident), None)
            lk = list(state.get("lock") or [])
        return {"kind": "agent", "title": (a or {}).get("label", ident), "agent": a, "lock": lk}
    if kind == "builder":
        b = (state.get("orchestrator") or {}).get("builder") or {}
        return {"kind": "builder", "title": "Builder (build pesado em lote)", "builder": b,
                "log": builder_log((b.get("running") or {}).get("dir")),
                "lock": heavy_lock_list()}
    if kind == "run":
        r = next((x for x in runs if x.get("id") == ident), None)
        if not r:
            return {"error": "run não encontrado"}
        t = next((x for x in tasks if x.get("id") == r.get("task_id")), None)
        return {"kind": "run", "title": "%s · %s" % (r.get("name") or r.get("id"), r.get("role") or ""),
                "run": {k: r.get(k) for k in ("id", "name", "role", "state", "started", "last_beat", "ended", "exit")},
                "task": task_view(t), "activity": run_activity(r.get("stream_path") or ""),
                "events": events_of({r.get("task_id")}, 10)}
    prnum = ident if kind == "pr" else None
    if kind == "pr":
        f = next((x for x in feats if str(x.get("pr")) == str(ident)), None)
        ident = f.get("id") if f else "pr-%s" % ident
        kind = "feature"
    f = next((x for x in feats if x.get("id") == ident or x.get("name") == ident), None)
    ts = [x for x in tasks if x.get("feature_id") == ident or x.get("id") == ident]
    train = next((x for x in ((state.get("orchestrator") or {}).get("train") or [])
                  if (f and str(x.get("pr")) == str(f.get("pr"))) or (prnum and str(x.get("pr")) == str(prnum))), None)
    pr_row = next((p for p in state.get("prs") or [] if str(p["number"]) == str(prnum)), None) if prnum else None
    rs = [x for x in runs if x.get("task_id") in {t.get("id") for t in ts}]
    return {"kind": "feature", "title": ("PR #%s: %s" % (prnum, (train or pr_row or {}).get("title", ""))) if prnum else ident,
            "gh": pr_row,
            "feature": {k: (f or {}).get(k) for k in ("id", "state", "track", "pr", "branch", "fixes", "partial", "blocked_reason", "job_reds")},
            "train": train, "tasks": [task_view(t) for t in ts][:6],
            "runs": [{k: x.get(k) for k in ("id", "name", "role", "state", "started", "ended", "exit")} for x in rs][-6:],
            "events": events_of({ident} | {t.get("id") for t in ts}, 20)}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/api/state"):
            with lock:
                body = json.dumps(state).encode()
            ctype = "application/json"
        elif self.path.startswith("/api/detail"):
            from urllib.parse import parse_qs, urlparse
            q = parse_qs(urlparse(self.path).query)
            try:
                d = detail((q.get("kind") or [""])[0], (q.get("id") or [""])[0])
            except Exception as e:
                d = {"error": str(e)}
            body, ctype = json.dumps(d).encode(), "application/json"
        else:
            body, ctype = open(PAGE_FILE, "rb").read(), "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--agents", default=os.environ.get("SONORA_DASH_AGENTS"))
    args = ap.parse_args()
    threading.Thread(target=refresh_loop, args=(args.agents,), daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("http://127.0.0.1:%d/" % args.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

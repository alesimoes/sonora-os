"""What the dashboard reads from the machine: GitHub (gh), docker, ps, the
agents' worktrees, the usage snapshots and the heavy-job lock
(tools/build-slot.sh). Read-only; every reader returns empty on failure."""
import json
import os
import re
import shutil
import subprocess
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


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


def orch_root():
    """The orchestrator's checkout: three levels above its status.json."""
    path = os.environ.get("SONORA_ORCH_STATUS") or os.path.join(ROOT, "planning", "orchestrator", "status.json")
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(path))))


# --- the machine-wide heavy lock -------------------------------------------
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


GIT_BASH = next((b for b in (r"C:\Program Files\Git\bin\bash.exe", shutil.which("bash")) if b and os.path.exists(b)), None)
SLOT_SCRIPT = _find_slot_script()


def heavy_lock_list():
    """Who holds the machine-wide heavy lock and who waits (tools/build-slot.sh)."""
    try:
        out = subprocess.run([GIT_BASH or "bash", SLOT_SCRIPT, "--list"],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=10).stdout
        return out.strip().splitlines()[:20]
    except (OSError, subprocess.SubprocessError):
        return []

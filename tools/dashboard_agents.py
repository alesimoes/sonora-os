"""The coordinator's agents (the --agents label file) and what their
worktrees and the heavy lock say about each one."""
import json
import os
import time

from dashboard_util import ROOT, norm, run

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

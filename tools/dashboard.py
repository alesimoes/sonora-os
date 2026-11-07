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
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from dashboard_orch import (build_time, builder_log, clean_orch, events_of, orch_state, read_coordinator,
                            read_orchestrator, read_progress, run_activity, task_view)
from dashboard_sources import (heavy_lock_list, read_agents, read_containers, read_lock, read_merged, read_prs,
                               read_usage, read_watchers, stamp_done)

state = {"usage": {"sessions": [], "week": None, "five_hour": None}, "updated": 0, "agents": [], "prs": [], "watchers": [], "containers": [], "errors": [], "orchestrator": None, "merged": [], "lock": []}
lock = threading.Lock()


def refresh_loop(agents_file):
    last_pr, prs, merged, gh_ok = 0, [], [], False
    last_prog, progress = 0, None
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
        try:
            bt = build_time(read_orchestrator())
        except Exception as e:
            bt = None
            errors.append("build time: %s" % e)
        if time.time() - last_prog > 300:
            try:
                progress = read_progress()
            except Exception as e:
                errors.append("progress: %s" % e)
            last_prog = time.time()
        orch = clean_orch(read_orchestrator(), {p["number"] for p in prs} if gh_ok else None)
        with lock:
            state.update(updated=int(time.time()), progress=progress, prs=prs, watchers=watchers, containers=containers, merged=merged, usage=read_usage(), orchestrator=orch, coordinator=read_coordinator(), lock=lk, buildtime=bt)
        agents = read_agents(labels, containers, lk)
        with lock:
            state.update(updated=int(time.time()), agents=agents, prs=prs, merged=merged,
                         watchers=watchers, containers=containers, errors=errors)
        time.sleep(4)


PAGE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")


# --- details for a clicked card ------------------------------------------
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

#!/usr/bin/env python3
"""Local live board: agents, merge train, local CI, disk, PRs, containers.

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
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from dashboard_chat import handle_post
from dashboard_detail import detail
from dashboard_links import MOCKUPS_ROUTE, mockup_file, read_links
from dashboard_state import lock, refresh_loop, state
from dashboard_static import STATIC_ROUTE, page, static_file

MAX_POST = 20000


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path
        if path.startswith("/api/state"):
            with lock:
                body = json.dumps(dict(state, links=read_links())).encode()
            ctype = "application/json"
        elif path.startswith("/api/detail"):
            q = parse_qs(urlparse(path).query)
            try:
                d = detail((q.get("kind") or [""])[0], (q.get("id") or [""])[0])
            except Exception as e:
                d = {"error": str(e)}
            body, ctype = json.dumps(d).encode(), "application/json"
        elif path.startswith(MOCKUPS_ROUTE) or path.startswith(STATIC_ROUTE):
            got = (mockup_file if path.startswith(MOCKUPS_ROUTE) else static_file)(path)
            if got is None:
                return self.reply(404, {"error": "not found"})
            body, ctype = got
        else:
            body, ctype = page()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        """Owner actions. A JSON content type forces a CORS preflight, which
        this server never answers, so another web page cannot post here."""
        host = (self.headers.get("Host") or "").split(":")[0]
        if host not in ("127.0.0.1", "localhost"):
            return self.reply(403, {"error": "forbidden host"})
        if "application/json" not in (self.headers.get("Content-Type") or ""):
            return self.reply(415, {"error": "send application/json"})
        try:
            n = int(self.headers.get("Content-Length") or 0)
            payload = json.loads(self.rfile.read(min(n, MAX_POST)) or b"{}")
        except ValueError:
            return self.reply(400, {"error": "bad JSON"})
        code, obj = handle_post(self.path.split("?", 1)[0], payload)
        self.reply(code, obj)

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

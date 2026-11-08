#!/usr/bin/env python3
"""Sonora Console: one screen, a conversation with the orchestrator.

    python tools/dashboard.py [--port 8765]

Listens on 127.0.0.1 only. Open http://127.0.0.1:8765/ . The design is
docs/design/dashboard-console/SPEC.md. The server reads status.json, GitHub
and the console's chat files; it changes nothing itself. A message or a tap
on Confirmar becomes a request at the daemon's door (harness/console_door.py)
and the daemon, which alone writes its state, decides and acts. Every POST
needs the per-start token (dashboard_token.py).
"""
import argparse
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import dashboard_console_http as console
import dashboard_selfupdate
from dashboard_state import refresh_loop
from dashboard_static import STATIC_ROUTE, page, static_file
from dashboard_token import TOKEN_HEADER, inject_token, token_ok

MAX_POST = 20000


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        route = self.path.split("?", 1)[0]
        if route == "/api/console":
            return self.reply(200, console.view())
        if route == "/api/console/stream":
            return self.stream()
        if route.startswith(STATIC_ROUTE):
            got = static_file(route)
            return self.send(*got) if got else self.reply(404, {"error": "not found"})
        body, ctype = page()
        self.send(inject_token(body), ctype)

    def stream(self):
        """Server-Sent Events: one thread per open page (ThreadingHTTPServer)."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        self.close_connection = True
        console.stream.serve(self.wfile, console.frames())

    def send(self, body, ctype, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def reply(self, code, obj):
        self.send(json.dumps(obj).encode(), "application/json", code)

    def do_POST(self):
        """A JSON content type forces a CORS preflight, which this server
        never answers, so another web page cannot post here."""
        host = (self.headers.get("Host") or "").split(":")[0]
        if host not in ("127.0.0.1", "localhost"):
            return self.reply(403, {"error": "forbidden host"})
        if "application/json" not in (self.headers.get("Content-Type") or ""):
            return self.reply(415, {"error": "send application/json"})
        if not token_ok(self.headers.get(TOKEN_HEADER)):
            return self.reply(403, {"error": "missing or wrong token: reload the page"})
        try:
            n = int(self.headers.get("Content-Length") or 0)
            payload = json.loads(self.rfile.read(min(n, MAX_POST)) or b"{}")
        except ValueError:
            return self.reply(400, {"error": "bad JSON"})
        self.reply(*console.handle_post(self.path.split("?", 1)[0], payload))

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--agents", help="ignored: kept so older launchers still work")
    args = ap.parse_args()
    threading.Thread(target=refresh_loop, daemon=True).start()
    if os.environ.get("SONORA_DASH_NO_SELFUPDATE") != "1":
        dashboard_selfupdate.start()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("http://127.0.0.1:%d/" % args.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()

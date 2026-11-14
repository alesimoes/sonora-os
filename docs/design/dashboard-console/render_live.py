#!/usr/bin/env python3
"""Screenshots of the REAL console page (tools/dashboard.html + static files)
fed by a stub /api/console built with the real dashboard_console.build and
made-up numbers. Usage: render_live.py [browser]  -> docs/images/dashboard-console/"""
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "..", "..", "tools")
OUT = os.path.join(HERE, "..", "..", "images", "dashboard-console")
sys.path.insert(0, TOOLS)
import dashboard_console as V  # noqa: E402
import dashboard_static as S  # noqa: E402

NOW = time.time()


def iso(ago):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(NOW - ago))


def prog():
    return {"pct": 52, "done": 359, "doing": 111, "total": 688, "tracks": [
        {"key": "daw", "title": "Complete DAW", "pct": 55, "done": 270, "doing": 70, "total": 490},
        {"key": "plugins", "title": "Plugins", "pct": 48, "done": 62, "doing": 26, "total": 130},
        {"key": "os", "title": "OS packaging", "pct": 27, "done": 27, "doing": 15, "total": 68}]}


def prs():
    rows = [(571, "console: instant send, no blinking", "verde", "daemon (merge automático)", 14),
            (572, "plugins: eco mode for the reverb", "rodando", "daemon, quando ficar verde", 6),
            (573, "engine: ring buffer overrun counter", "vermelho", "eng-104 (corretor)", 41),
            (574, "os: installer layout test", "conflito", "ninguém ainda: falta integrador", 95)]
    return [{"number": n, "title": t, "gate": g, "gate_text": g, "owner": o, "opened_min": m} for n, t, g, o, m in rows]


def snap():
    ag = [{"name": "eng-119", "what": "dash.console-kpis", "model": "sonnet", "role_pt": "Engenheiro",
           "since": NOW - 1500, "last": NOW - 5}]
    return {"orchestrator": {"updated_ts": NOW - 6, "cap": 8, "runs_last_hour": 3, "max_runs_per_hour": 6,
                             "open_prs": 4, "max_open_prs": 8,
                             "train": [{"pr": 572, "phase": "esperando checks"}]},
            "board": {"agents": ag, "prs": prs()}, "progress": prog(),
            "merged": [{"mergedAt": iso(s)} for s in (600, 2400, 9000, 20000, 30000)]}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        route = self.path.split("?", 1)[0]
        if route == "/api/console":
            body = V.build(snap(), True, NOW)
            body["chat"] = {"messages": [], "actions": {}}
            return self.send(json.dumps(body).encode(), "application/json")
        if route.startswith(S.STATIC_ROUTE):
            return self.send(*S.static_file(route))
        page, ctype = S.page()
        pane = self.path.split("pane=")[-1] if "pane=" in self.path else "chat"
        page = page.replace(b"</body>", b'<script>document.body.dataset.pane="%s"</script></body>' % pane.encode())
        self.send(page, ctype)

    def send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    br = (sys.argv[1:] or [os.environ.get("BROWSER") or "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"])[0]
    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d/" % srv.server_port
    for name, size, pane in (("celular-numeros", "520,900", "numeros"), ("celular-chat-kpis", "520,900", "chat"),
                             ("desktop-chat-kpis", "1440,900", "chat")):
        subprocess.run([br, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--virtual-time-budget=4000",
                        "--window-size=" + size, "--screenshot=" + os.path.abspath(os.path.join(OUT, name + ".png")),
                        base + "?pane=" + pane], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60)
    srv.shutdown()


if __name__ == "__main__":
    main()

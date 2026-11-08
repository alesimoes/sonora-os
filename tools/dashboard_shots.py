#!/usr/bin/env python3
"""Screenshots of the dashboard from a made-up snapshot (no daemon, GitHub or
Docker needed): serves tools/dashboard.html with canned state and has a
headless Chrome/Edge photograph it.

    python tools/dashboard_shots.py docs/images/dashboard-board.png [--height 1500]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
BROWSERS = (r"C:\Program Files\Google\Chrome\Application\chrome.exe",
            r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")
NOW = time.time()


def pr(n, title, **kw):
    return dict({"number": n, "title": title, "branch": "feat/x%d" % n, "merge": "CLEAN", "mergeable": "MERGEABLE",
                 "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(NOW - 3600)), "failed": [], "pending": [],
                 "passed": 4, "total": 4, "cancelled": [], "draft": False, "base": "main",
                 "failed_details": []}, **kw)


def fake_orch():
    runs = [{"id": "r1", "name": "principal-2", "role": "principal", "task": "pr-515", "pr": 515,
             "title": "Corrigir o gate vermelho do PR 515", "started": NOW - 2400, "last_beat": NOW - 40,
             "last_step": "Bash: pytest tools/harness/tests", "building": False, "suspect": False},
            {"id": "r2", "name": "eng-89", "role": "engineer", "task": "dash.everything-that-runs", "pr": None,
             "title": "Painel: tudo que está rodando e por que o pipeline está segurado", "started": NOW - 900,
             "last_beat": NOW - 12, "last_step": "Edit: tools/dashboard_board.py", "building": True, "suspect": False},
            {"id": "r3", "name": "fixer-4", "role": "fixer", "task": "pr-530", "pr": 530, "title": "Conflito com a main no PR 530",
             "started": NOW - 300, "last_beat": NOW - 700, "last_step": "", "building": False, "suspect": True}]
    pools = {"build:daw": {"floor": 8, "live": 1, "reason": "8 open PRs: merging first", "paused": False},
             "build:os": {"floor": 4, "live": 0, "reason": "paused", "paused": True},
             "build:plugin": {"floor": 2, "live": 0, "reason": "files claimed or dependencies pending", "paused": False}}
    return {"updated_ts": NOW - 3, "mode": "live", "cap": 10, "live": 3, "paused": ["build:os"], "open_prs": 8,
            "max_open_prs": 8, "runs_last_hour": 41, "max_runs_per_hour": 60, "pools": pools, "runs": runs,
            "features": [{"id": "dash.everything-that-runs", "pr": None, "tasks": [{"name": "dash.everything-that-runs"}]}],
            "train": [{"pr": 515, "fixer": "principal-2"}, {"pr": 530, "fixer": "fixer-4"}],
            "alerts": [{"id": "lock", "level": "alert", "check": "stall_check", "since": NOW - 1500,
                        "text": "o agente fixer-4 está sem sinal há 12 min"}]}


def fake_state():
    import dashboard_board as B
    import dashboard_brakes as K
    import dashboard_prboard as P
    prs = [pr(515, "Gate vermelho no micro build", failed=["micro"], passed=2, total=3),
           pr(530, "Conflito nas rotas do mixer", merge="DIRTY"),
           pr(541, "Painel: tudo que roda"),
           pr(542, "ADR do formato de sessão", pending=["micro"], passed=1, total=3),
           pr(543, "Docs: guia de teclas")]
    orch = fake_orch()
    clock = P.GreenClock()
    clock.since[541] = NOW - 14 * 60
    coord = [{"id": "tu1", "role": "principal-engineer", "name": "principal-engineer", "doing": "Corrigir o PR #515",
              "pr": 515, "since": NOW - 1500, "last": NOW - 30, "step": "Read: tools/harness/train.py"},
             {"id": "tu2", "role": "code-reviewer", "name": "code-reviewer", "doing": "Revisar o PR 543",
              "pr": 543, "since": NOW - 200, "last": NOW - 8, "step": "Bash: git diff origin/main"}]
    return {"usage": {"sessions": [], "week": None, "five_hour": None}, "updated": int(NOW), "agents": [], "prs": prs,
            "watchers": [], "containers": [], "errors": [], "orchestrator": orch, "merged": [], "lock": [], "disk": None,
            "coordinator": {"updated": NOW - 20, "doing": "Acompanhando o PR 515 e o painel"},
            "board": {"agents": B.agent_rows(orch, coord, [], NOW), "prs": P.pr_rows(prs, orch, clock, NOW),
                      "brakes": K.brakes(orch, NOW)}}


def serve(port):
    import dashboard
    import dashboard_state
    with dashboard_state.lock:
        dashboard_state.state.update(fake_state())
    srv = ThreadingHTTPServer(("127.0.0.1", port), dashboard.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def browser():
    found = next((b for b in BROWSERS if os.path.exists(b)), None) or shutil.which("chrome") or shutil.which("msedge")
    if not found:
        sys.exit("no Chrome or Edge found")
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--height", type=int, default=1500)
    ap.add_argument("--port", type=int, default=8791)
    args = ap.parse_args()
    tmp = tempfile.mkdtemp()
    os.environ["SONORA_ORCH_STATUS"] = os.path.join(tmp, "planning", "orchestrator", "status.json")
    srv = serve(args.port)
    try:
        subprocess.run([browser(), "--headless", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
                        "--user-data-dir=" + os.path.join(tmp, "profile"),
                        "--window-size=1500,%d" % args.height, "--virtual-time-budget=6000",
                        "--screenshot=" + os.path.abspath(args.out), "http://127.0.0.1:%d/" % args.port],
                       check=True, timeout=120, capture_output=True)
    finally:
        srv.shutdown()
        shutil.rmtree(tmp, True)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""What the `console` agent may read (owner, 2026-11-08): small, bounded views
of the orchestrator's exported files, so it never loads state.json or
events.jsonl whole. Read-only; prints Portuguese-friendly plain text.

    console_read.py inbox | status | agents | prs | why PR | events [N] | task ID

Run from the orchestrator's checkout (the agent's working directory).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import console_store as S  # noqa: E402
from harness import whynot  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ODIR = os.path.join(ROOT, "planning", "orchestrator")
MAX_EVENTS = 40


def hdir():
    return os.environ.get("SONORA_HARNESS") or os.path.join(ROOT, ".harness")


def status():
    try:
        with open(os.path.join(ODIR, "status.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def events(n=MAX_EVENTS):
    """Newest first; only the file's tail is read."""
    path = os.path.join(ODIR, "events.jsonl")
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 300000))
            lines = f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []
    out = []
    for ln in reversed(lines):
        try:
            out.append(json.loads(ln))
        except ValueError:
            continue
        if len(out) >= n:
            break
    return out


def cmd_inbox():
    rows = S.history(hdir())
    pend = S.unanswered(hdir())
    print("MENSAGENS SEM RESPOSTA (id: texto):")
    for m in pend:
        print(f"  {m['id']}: {m['text']}")
    if not pend:
        print("  (nenhuma)")
    print("ÚLTIMAS TROCAS:")
    for m in rows[-8:]:
        print(f"  [{m['kind']}] {m['text'][:200]}")


def cmd_status():
    s = status()
    print(f"daemon atualizado: {s.get('updated')}  modo: {s.get('mode')}  "
          f"chave: {s.get('kill_switch') or 'livre'}  pausados: {s.get('paused')}")
    print(f"agentes: {s.get('live')}/{s.get('cap')}  execuções/h: "
          f"{s.get('runs_last_hour')}/{s.get('max_runs_per_hour')}  PRs abertos: "
          f"{s.get('open_prs')}/{s.get('max_open_prs')}  freios: {s.get('budget_holds')}")
    print(f"à espera do dono: {len(s.get('waits_on_owner') or [])}  "
          f"bloqueados: {sum(len(g.get('items') or []) for g in s.get('blocked') or [])}")


def cmd_agents():
    for r in status().get("runs") or []:
        print(f"{r.get('name')} role={r.get('role')} task={r.get('task')} "
              f"model={r.get('model')} started={r.get('started')} "
              f"last_beat={r.get('last_beat')} suspect={r.get('suspect')}")


def cmd_prs():
    for t in status().get("train") or []:
        print(f"#{t.get('pr')} {str(t.get('title'))[:60]} estado={t.get('state')} "
              f"fase={t.get('phase')} checks={t.get('passed')}/{t.get('total')} "
              f"falhando={t.get('failing')} agente={t.get('fixer')} próximo={t.get('next')}")


def cmd_why(n):
    s = status()
    row = next((r for r in s.get("train") or [] if r.get("pr") == n), None)
    pr = {"number": n, "branch": (row or {}).get("branch"), "total": (row or {}).get("total"),
          "passed": (row or {}).get("passed"), "failed": (row or {}).get("failing") or []}
    up = bool(s.get("updated_ts"))
    out = whynot.explain(pr, s, events(200), up)
    print(f"PR #{n}: {out['reason']}")
    print(f"dono: {out['owner']}  checks: {out['gate']}  trem: {out['on_train']}")
    print(f"último evento: {out['event']}")


def cmd_events(n):
    for e in events(min(n, MAX_EVENTS)):
        print(f"{e.get('ts')} {e.get('task_id') or '-'} {e.get('kind')} {str(e.get('detail'))[:140]}")


def cmd_task(tid):
    s = status()
    for t in s.get("tasks") or []:
        if t.get("id") == tid:
            print(json.dumps(t, ensure_ascii=False))
            return
    print(f"tarefa {tid}: não achei")


def main(argv):
    # The agent's pipe on Windows defaults to cp1252; the log showed mojibake.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    cmd, arg = (argv + [None, None])[:2]
    num = int(arg.lstrip("#")) if arg and arg.lstrip("#").isdigit() else None
    table = {"inbox": cmd_inbox, "status": cmd_status, "agents": cmd_agents, "prs": cmd_prs}
    if cmd in table:
        table[cmd]()
    elif cmd == "why" and num:
        cmd_why(num)
    elif cmd == "events":
        cmd_events(num or 20)
    elif cmd == "task" and arg:
        cmd_task(arg)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

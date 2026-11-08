"""What the dashboard reads about one orchestrator run or task: the event
log, a run's stream-json log, and a task's summary."""
import json
import os

from dashboard_util import orch_root

def tail_lines(path, max_bytes=600000):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            start = max(0, f.tell() - max_bytes)
            f.seek(start)
            lines = f.read().decode("utf-8", "replace").splitlines()
            return lines[1:] if start else lines   # the first line is cut only when we seeked into it
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

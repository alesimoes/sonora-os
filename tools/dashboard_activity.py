"""What one running agent is doing: its last steps, read from its run's
stream.log (owner, 2026-11-09: "a gente clica no agente e nada acontece, não
consigo ver o que está acontecendo").

    GET /api/agent/<name>/activity  -> {"name", "task", "steps": [...]}

A step is {"ts", "kind": "tool"|"text", "tool"?, "text"}; newest last. Only the
tail of the log is read, so a long run costs the same as a short one. Nothing
here writes anything.
"""
import json
import os
import re

TAIL_BYTES = 256 * 1024
MAX_STEPS = 25
MAX_TEXT = 240
# The input field that says best what a tool call is doing, in order.
SUMMARY_KEYS = ("description", "command", "file_path", "pattern", "path", "url", "query", "prompt")


# An agent's own worktree prefix says nothing; the path inside it does.
WORKTREE = re.compile(r"[^\s'\"]*[\\/]\.claude[\\/]worktrees[\\/]"
                      r"(?:[^\\/\s]+[\\/]\.claude[\\/]worktrees[\\/])?[^\\/\s]+[\\/]?")


def _short(s, n=MAX_TEXT):
    s = " ".join(WORKTREE.sub("", str(s)).split())
    return s if len(s) <= n else s[:n - 1] + "…"


def _tool_summary(name, inp):
    inp = inp if isinstance(inp, dict) else {}
    if name.endswith("__send"):
        body = inp.get("body")
        what = body.get("text") or body.get("summary") if isinstance(body, dict) else body
        return _short(f"→ {inp.get('to', '?')} ({inp.get('type', '')}) {what or ''}")
    if name.endswith("__recv"):
        return "esperando mensagem"
    for k in SUMMARY_KEYS:
        if inp.get(k):
            return _short(inp[k])
    vals = [v if isinstance(v, str) else json.dumps(v) for v in inp.values()]
    return _short(" ".join(vals))


def _steps_of(line):
    try:
        ev = json.loads(line)
    except ValueError:
        return []
    if not isinstance(ev, dict) or ev.get("type") != "assistant":
        return []
    content = (ev.get("message") or {}).get("content") or []
    ts = ev.get("timestamp") or ""
    out = []
    for c in content if isinstance(content, list) else []:
        if not isinstance(c, dict):
            continue
        if c.get("type") == "text" and str(c.get("text") or "").strip():
            out.append({"ts": ts, "kind": "text", "text": _short(c["text"])})
        elif c.get("type") == "tool_use":
            name = str(c.get("name") or "")
            out.append({"ts": ts, "kind": "tool", "tool": name.split("__")[-1],
                        "text": _tool_summary(name, c.get("input"))})
    return out


def _tail_lines(path, size=TAIL_BYTES):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        end = f.tell()
        f.seek(max(0, end - size))
        data = f.read()
    lines = data.decode("utf-8", "replace").splitlines()
    return lines[1:] if end > size else lines   # the first one may be cut


def steps(stream_path, limit=MAX_STEPS):
    """The last `limit` steps of a stream.log; [] when it cannot be read."""
    try:
        lines = _tail_lines(stream_path)
    except OSError:
        return []
    out = []
    for line in lines:
        out.extend(_steps_of(line))
    return out[-limit:]


def find_run(orch, name):
    for r in (orch or {}).get("runs") or []:
        if isinstance(r, dict) and r.get("name") == name:
            return r
    return None


def activity(orch, name):
    """(status, body) for GET /api/agent/<name>/activity."""
    run = find_run(orch, name)
    if run is None:
        return 404, {"error": "agente não está rodando"}
    path = run.get("stream_path") or ""
    return 200, {"name": name, "task": run.get("task") or "", "title": run.get("title") or "",
                 "model": run.get("model") or "", "steps": steps(path) if path else []}

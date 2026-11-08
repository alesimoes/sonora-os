"""The coordinator session's own subagents (Agent-tool runs), recorded for the
dashboard. The daemon knows nothing about them, so Claude Code hooks
(tools/harness/coord_hook.py) feed this file:

    <project>/.harness/coordinator-agents.json
    {"updated": ts, "agents": {"<tool_use_id>": {...row...}}}

Pure functions over the parsed hook payload and the file's dict; the I/O is in
coord_hook.py. A row: id, agent_id, role, name, doing, pr, since, last,
step, background."""
import re
import time

STALE_S = 6 * 3600       # a row nobody touched for this long is dropped
AGENT_TOOLS = ("Agent", "Task")
_PR = re.compile(r"(?:PR\s*|#)(\d{1,5})\b", re.I)
STEP_MAX = 200


def first_line(text, limit=240):
    for line in str(text or "").splitlines():
        line = line.strip(" #>*-`\t")
        if line:
            return line[:limit]
    return ""


def pr_in(*texts):
    """The first PR number a description or brief mentions, or None."""
    for t in texts:
        m = _PR.search(str(t or ""))
        if m:
            return int(m.group(1))
    return None


def launch_row(payload, now):
    inp = payload.get("tool_input") or {}
    role = inp.get("subagent_type") or "general-purpose"
    desc = inp.get("description") or first_line(inp.get("prompt"))
    return {"id": payload.get("tool_use_id") or "%s-%d" % (role, now),
            "agent_id": None, "role": role, "name": role, "doing": desc,
            "pr": pr_in(desc, inp.get("prompt")), "since": now, "last": now,
            "step": "", "model": inp.get("model"),
            "background": bool(inp.get("run_in_background"))}


def step_text(payload):
    inp = payload.get("tool_input") or {}
    detail = inp.get("description") or inp.get("command") or \
        inp.get("file_path") or inp.get("pattern") or ""
    return ("%s: %s" % (payload.get("tool_name") or "?",
                        " ".join(str(detail).split())))[:STEP_MAX]


def agent_id_of(payload):
    """The id of the subagent that produced this event, or the id a
    background launch answered with."""
    if payload.get("agent_id"):
        return str(payload["agent_id"])
    resp = payload.get("tool_response")
    m = re.search(r"agentId[\"':\s]+([\w-]+)", str(resp or ""))
    return m.group(1) if m else None


def find_row(agents, agent_id, role=None):
    """The row of agent_id; a first sighting claims the oldest row of that
    role that no agent id owns yet."""
    if not agent_id:
        return None
    for k, r in agents.items():
        if r.get("agent_id") == agent_id:
            return k
    free = [(r["since"], k) for k, r in agents.items()
            if not r.get("agent_id") and (role is None or r["role"] == role)]
    return min(free)[1] if free else None


def drop_stale(agents, now):
    return {k: r for k, r in agents.items() if now - r.get("last", 0) < STALE_S}


def apply_event(agents, payload, now=None):
    """The new agents dict after one hook event (the input is not changed)."""
    now = now or time.time()
    agents = drop_stale({k: dict(v) for k, v in agents.items()}, now)
    event, tool = payload.get("hook_event_name"), payload.get("tool_name")
    if tool in AGENT_TOOLS:
        return _agent_tool_event(agents, payload, event, now)
    if event == "SubagentStop":
        agents.pop(find_row(agents, agent_id_of(payload), payload.get("agent_type")), None)
        return agents
    aid = payload.get("agent_id")
    k = find_row(agents, aid, payload.get("agent_type"))
    if k:
        agents[k].update(agent_id=str(aid), last=now, step=step_text(payload))
    return agents


def _agent_tool_event(agents, payload, event, now):
    row = launch_row(payload, now)
    if event == "PreToolUse":
        agents.setdefault(row["id"], row)
    elif event == "PostToolUse":
        cur = agents.get(row["id"])
        if cur and not cur["background"]:
            del agents[row["id"]]            # a foreground run ended
        elif cur:
            cur.update(agent_id=agent_id_of(payload) or cur["agent_id"], last=now)
    return agents

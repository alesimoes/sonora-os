"""Shadow rollout (ADR-0047 section 7): the daemon runs dry beside the
in-session coordinator and logs where its launch decisions differ from the
agents the coordinator really runs.

The coordinator maintains a JSON file (--shadow-agents):

    {"updated": "<iso>", "agents": [
        {"name": "ui-engineer-3", "role": "engineer", "task": "<task id,
         feature id or BACKLOG id>", "branch": "feat/x", "track": "daw"}]}

A bare list of agent objects is accepted too. Lines go to shadow.log only
when the picture changes, so a day of agreement is a short file.
"""
import json
import os
import time


def load_agents(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return None
    if isinstance(data, dict):
        data = data.get("agents", [])
    return [a for a in data if isinstance(a, dict)] \
        if isinstance(data, list) else None


def _keys(task):
    """Every name a live agent may use for this task."""
    ids = (task.get("backlog_id") or "").split(",")
    return {k for k in (task["id"], task.get("feature_id"), task.get("branch"),
                        *ids) if k}


def _agent_keys(a):
    return {k for k in (a.get("task"), a.get("feature"), a.get("branch"))
            if k}


class Shadow:
    def __init__(self, log_path, agents_path, clock=time.time):
        self.log_path, self.agents_path, self.clock = \
            log_path, agents_path, clock
        self.last = None

    def compare(self, tasks, launches, floors, live_by_track=None):
        """Returns the list of difference lines (empty when they agree)."""
        agents = load_agents(self.agents_path)
        if agents is None:
            return [f"agents file {self.agents_path} unreadable"]
        by_id = {t["id"]: t for t in tasks}
        diffs, matched = [], set()
        for ln in launches:
            t = by_id[ln.task_id]
            hit = next((i for i, a in enumerate(agents)
                        if _agent_keys(a) & _keys(t)), None)
            if hit is None:
                diffs.append(f"DAEMON_ONLY would launch {ln.kind} for "
                             f"{t['id']} ({ln.pool}); no live agent on it")
            else:
                matched.add(hit)
        known = set().union(*(_keys(t) for t in tasks)) if tasks else set()
        for i, a in enumerate(agents):
            if i in matched:
                continue
            if not _agent_keys(a) & known:
                diffs.append(f"LIVE_ONLY {a.get('name', '?')} "
                             f"({a.get('role', '?')}) on "
                             f"{a.get('task') or a.get('branch')}: not a "
                             "task the daemon knows")
        per_track = {}
        for a in agents:
            if a.get("role", "engineer") == "engineer":
                per_track[a.get("track")] = per_track.get(a.get("track"), 0) + 1
        for track, floor in sorted(floors.items()):
            have = per_track.get(track, 0)
            if have < floor:
                diffs.append(f"POOL {track}: coordinator has {have} "
                             f"engineers, floor {floor}")
        return diffs

    def record(self, tasks, launches, floors):
        diffs = self.compare(tasks, launches, floors)
        if diffs == self.last:
            return diffs
        self.last = diffs
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(self.clock()))
        with open(self.log_path, "a", encoding="utf-8") as f:
            if not diffs:
                f.write(f"{stamp} AGREE ({len(launches)} launch decisions)\n")
            for d in diffs:
                f.write(f"{stamp} {d}\n")
        return diffs

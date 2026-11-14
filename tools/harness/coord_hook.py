#!/usr/bin/env python3
"""Claude Code hook: records the coordinator session's subagents in
<project>/.harness/coordinator-agents.json for the dashboard (see
coord_agents.py). Reads the hook payload on stdin. It never fails the
session: any error ends in exit 0 and the board simply misses a row.

Wire it in the coordinator's .claude/settings.json (PreToolUse matcher
"Agent|Task", PostToolUse matcher ".*": a subagent's own tool calls are its
heartbeat, and SubagentStop), each as

    {"type": "command", "timeout": 5,
     "command": "python \"$CLAUDE_PROJECT_DIR/tools/harness/coord_hook.py\""}

SONORA_COORD_DIR overrides the folder (default <project>/.harness); the
dashboard reads it under the orchestrator's checkout and the main checkout."""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from harness.coord_agents import apply_event  # noqa: E402

FILE = "coordinator-agents.json"
LOCK_TRIES = 40


def target(env=None):
    env = os.environ if env is None else env
    base = env.get("SONORA_COORD_DIR") or os.path.join(
        env.get("CLAUDE_PROJECT_DIR") or os.getcwd(), ".harness")
    return os.path.join(base, FILE)


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f).get("agents") or {}
    except (OSError, ValueError, AttributeError):
        return {}


def save(path, agents, now):
    tmp = "%s.tmp.%d" % (path, os.getpid())
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"updated": now, "agents": agents}, f)
    os.replace(tmp, path)


def with_lock(path, fn):
    """Hooks of parallel subagents overlap: a lock file keeps their
    read-modify-write apart. A stuck lock is overridden after ~2 s."""
    lock = path + ".lock"
    for _ in range(LOCK_TRIES):
        try:
            os.close(os.open(lock, os.O_CREAT | os.O_EXCL))
            break
        except FileExistsError:
            time.sleep(0.05)
    try:
        fn()
    finally:
        try:
            os.remove(lock)
        except OSError:
            pass


def record(payload, path, now=None):
    now = now or time.time()
    os.makedirs(os.path.dirname(path), exist_ok=True)

    def work():
        save(path, apply_event(load(path), payload, now), now)
    with_lock(path, work)


def main():
    try:
        record(json.load(sys.stdin), target())
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

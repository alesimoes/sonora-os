#!/usr/bin/env bash
# Starts the local agent dashboard (http://127.0.0.1:8765/) when nothing is
# listening there yet, detached so it outlives the session. Called by the
# SessionStart hook; safe to run any number of times.
#
#   SONORA_DASH_AGENTS   agents label file (default ~/.claude/dashboard-agents.json)
#   SONORA_ORCH_STATUS   orchestrator status.json (default: the live daemon's worktree)
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port=8765

if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$port/"; then
  exit 0
fi
command -v python >/dev/null 2>&1 || { echo "dashboard-up: no python" >&2; exit 0; }

agents="${SONORA_DASH_AGENTS:-$HOME/.claude/dashboard-agents.json}"
orch_default="$root/.claude/worktrees/agent-a7018d355a2572f2a/planning/orchestrator/status.json"
export SONORA_ORCH_STATUS="${SONORA_ORCH_STATUS:-$orch_default}"
log="${TMPDIR:-/tmp}/sonora-dashboard.log"

if command -v cygpath >/dev/null 2>&1; then
  agents="$(cygpath -w "$agents")"
  SONORA_ORCH_STATUS="$(cygpath -w "$SONORA_ORCH_STATUS")"
fi
cd "$root" || exit 0
nohup python tools/dashboard.py --port "$port" --agents "$agents" >"$log" 2>&1 &
echo "dashboard: http://127.0.0.1:$port/ (started)"

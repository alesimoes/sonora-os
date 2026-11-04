#!/bin/sh
# Put pull requests in the merge queue the moment they are opened:
#
#   tools/enqueue-pr.sh PR...        start one watcher per PR, return at once
#   tools/enqueue-pr.sh --cancel PR... stop them (a blocking review finding)
#
# The queue is tools/merge-when-green.sh, one detached process per PR, which
# updates the branch when main moved and merges when the checks are green. It
# is never left waiting for hours: it gives up after SONORA_MERGE_DEADLINE
# (default 4 h) and a second start for the same PR is refused by its lock.
# Logs: ${SONORA_MERGE_LOGS:-${TMPDIR:-/tmp}/sonora-merge-logs}/merge-PR.log
set -u
here=$(dirname "$0")
logs=${SONORA_MERGE_LOGS:-${TMPDIR:-/tmp}/sonora-merge-logs}
locks=${TMPDIR:-/tmp}/sonora-merge-locks
mkdir -p "$logs"
cancel=0
[ "${1:-}" = --cancel ] && { cancel=1; shift; }
[ $# -ge 1 ] || { echo "usage: tools/enqueue-pr.sh [--cancel] PR..." >&2; exit 2; }
for pr in "$@"; do
  if [ "$cancel" = 1 ]; then
    pid=$(cat "$locks/$pr.lock/pid" 2>/dev/null) && kill "$pid" 2>/dev/null
    rm -rf "$locks/$pr.lock"
    echo "#$pr: cancelled"
    continue
  fi
  nohup "$here/merge-when-green.sh" "$pr" >"$logs/merge-$pr.log" 2>&1 &
  echo "#$pr: queued, log $logs/merge-$pr.log"
done

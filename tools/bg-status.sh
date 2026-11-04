#!/bin/sh
# What is still running that this project started: merge watchers and docker
# builds, with their age. The coordinator runs it at the end of every turn.
#
#   tools/bg-status.sh          list
#   tools/bg-status.sh --kill   kill duplicate watchers and docker runs whose
#                               worktree is gone or merged
# shellcheck disable=SC2009  # pgrep is not on every Git Bash
set -u
kill_mode=0
[ "${1:-}" = --kill ] && kill_mode=1

echo "== merge-when-green"
ps -ef | grep -E ' (/bin/)?sh (\./)?tools/merge-when-green\.sh ' | awk '{print $2, $5, substr($0, index($0,"tools/"))}'
dups=$(ps -ef | grep -E ' (/bin/)?sh (\./)?tools/merge-when-green\.sh ' | sed 's/.*merge-when-green.sh//' | sort | uniq -d)
[ -z "$dups" ] || echo "DUPLICATE watchers for:$dups"
if [ "$kill_mode" = 1 ] && [ -n "$dups" ]; then
  for args in $dups; do
    ps -ef | grep -E " sh (\./)?tools/merge-when-green\.sh $args\$" | awk '{print $2}' | sort -n | tail -n +2 | xargs -r kill
  done
fi

echo "== docker runs"
ps -ef | grep '[d]ocker.* run ' | awk '{print $2, $5}'
docker ps --format '{{.ID}} {{.Image}} {{.RunningFor}}' 2>/dev/null
if [ "$kill_mode" = 1 ]; then
  docker ps --filter ancestor=sonora-build:trixie -q 2>/dev/null | xargs -r docker kill
fi

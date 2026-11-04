#!/bin/sh
# Merge approved pull requests once their checks are green, one at a time:
#
#   tools/merge-when-green.sh PR...
#
# For each PR in turn: one that conflicts with main is skipped with the files
# named; one behind main is brought up to date and its checks waited for; it
# is squash-merged when no check failed, and skipped with the failing checks
# named otherwise. The integrator agent is for the skipped ones.
#
# The remote branch is deleted with git push, not gh's --delete-branch, which
# also deletes the local branch and fails while a worktree has it checked out.
# Exits 1 if any PR was skipped.
set -u

# shellcheck source=/dev/null
. "$(dirname "$0")/merge-checks.sh"

[ $# -ge 1 ] || { echo "usage: tools/merge-when-green.sh PR..." >&2; exit 2; }

# One watcher per PR, and none that outlives its purpose: a second launch for
# the same PR exits at once, and every watcher gives up after
# SONORA_MERGE_DEADLINE seconds (default 4 h) instead of idling for days.
lockroot=${TMPDIR:-/tmp}/sonora-merge-locks
mkdir -p "$lockroot"
mylocks=""
# shellcheck disable=SC2329  # runs from the trap
release() { for l in $mylocks; do rm -rf "$l"; done; }
trap release EXIT INT TERM
for pr in "$@"; do
  lock=$lockroot/$pr.lock
  if ! mkdir "$lock" 2>/dev/null; then
    if kill -0 "$(cat "$lock/pid" 2>/dev/null)" 2>/dev/null; then
      echo "#$pr: another merge-when-green (pid $(cat "$lock/pid")) already watches it; exiting" >&2
      exit 3
    fi
    rm -rf "$lock"; mkdir "$lock"
  fi
  echo $$ > "$lock/pid"
  mylocks="$mylocks $lock"
done
deadline=$(( $(date +%s) + ${SONORA_MERGE_DEADLINE:-14400} ))
expired() { [ "$(date +%s)" -ge "$deadline" ]; }

skipped=0
skip() {  # PR, reason
  echo "#$1: skipped: $2" >&2
  skipped=1
}

for pr in "$@"; do
  if expired; then skip "$pr" "deadline reached while waiting"; continue; fi
  # GitHub computes mergeability after a push; UNKNOWN until it has.
  tries=0
  while :; do
    state=$(gh pr view "$pr" --json mergeable,mergeStateStatus,headRefName \
      --jq '.mergeable + " " + .mergeStateStatus + " " + .headRefName') || state=""
    case "$state" in
      UNKNOWN*) ;;
      *) break ;;
    esac
    tries=$((tries + 1))
    [ "$tries" -lt 10 ] || break
    sleep 3
  done
  [ -n "$state" ] || { skip "$pr" "gh pr view failed"; continue; }
  read -r mergeable status branch <<EOF
$state
EOF

  if [ "$mergeable" = CONFLICTING ]; then
    if ! git fetch -q origin main || ! git fetch -q origin "pull/$pr/head"; then
      skip "$pr" "conflicts with main (could not fetch it to name the files)"
      continue
    fi
    files=$(git merge-tree --write-tree --name-only --no-messages origin/main FETCH_HEAD |
      tail -n +2)
    skip "$pr" "conflicts with main in:
$files"
    continue
  fi
  if [ "$status" = BEHIND ]; then
    gh pr update-branch "$pr" || { skip "$pr" "gh pr update-branch failed"; continue; }
    # Long enough for the push's checks to register, so the watch below
    # waits for them and not for the previous head's.
    sleep 30
  fi

  # The heavy jobs' check runs appear only once "Changed paths" finishes, so
  # a watch can end with only the cheap ones listed. Keep watching, for up to
  # 10 minutes, until every heavy check is present (passed or skipped).
  tries=0
  while :; do
    timeout 3600 gh pr checks "$pr" --watch --interval 60 >/dev/null 2>&1 || true
    expired && break
    missing=$(gh pr checks "$pr" --json name,state \
      --jq '.[] | [.name, .state] | @tsv' 2>/dev/null | missing_heavy_checks)
    [ -n "$missing" ] || break
    # A failed check is reported below, not waited out.
    gh pr checks "$pr" --json state --jq '.[].state' 2>/dev/null |
      grep -qx -e FAILURE -e CANCELLED -e ERROR -e TIMED_OUT -e STARTUP_FAILURE && break
    tries=$((tries + 1))
    [ "$tries" -lt 10 ] || break
    expired && break
    sleep 60
  done
  # The loop's snapshot only decides when to stop waiting: a push since then
  # could leave it describing an earlier head. merge_gate decides afresh.
  if ! head=$(merge_gate "$pr"); then
    skip "$pr" "$head"
    continue
  fi

  gh pr merge "$pr" --squash --match-head-commit "$head" || { skip "$pr" "gh pr merge failed"; continue; }
  # Deleting a branch another open PR is based on makes GitHub close that PR
  # (it happened to a stacked PR). Keep the branch while any open PR targets it.
  dependents=$(gh pr list --state open --base "$branch" --json number --jq '.[].number' 2>/dev/null | tr '
' ' ')
  if [ -n "$dependents" ]; then
    echo "#$pr: merged; kept branch $branch, open PRs are based on it: $dependents(retarget them to main)" >&2
  else
    git push -q origin --delete "$branch" ||
      echo "#$pr: merged; the remote branch $branch was not deleted" >&2
  fi
  echo "#$pr: merged"
done
exit "$skipped"

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

# shellcheck source=tools/merge-checks.sh
. "$(dirname "$0")/merge-checks.sh"

[ $# -ge 1 ] || { echo "usage: tools/merge-when-green.sh PR..." >&2; exit 2; }

skipped=0
skip() {  # PR, reason
  echo "#$1: skipped: $2" >&2
  skipped=1
}

for pr in "$@"; do
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
    gh pr checks "$pr" --watch --interval 60 >/dev/null 2>&1 || true
    missing=$(gh pr checks "$pr" --json name,bucket \
      --jq '.[] | [.name, .bucket] | @tsv' 2>/dev/null | missing_heavy_checks)
    [ -n "$missing" ] || break
    # A failed check is reported below, not waited out.
    gh pr checks "$pr" --json bucket --jq '.[].bucket' 2>/dev/null |
      grep -qx -e fail -e cancel && break
    tries=$((tries + 1))
    [ "$tries" -lt 10 ] || break
    sleep 60
  done
  # Read before the checks are, so a push after this point makes the merge
  # refuse rather than land a head nobody checked.
  head=$(gh pr view "$pr" --json headRefOid --jq .headRefOid) || {
    skip "$pr" "gh pr view failed"; continue; }
  total=$(gh pr checks "$pr" --json name --jq 'length' 2>/dev/null) || total=0
  [ "${total:-0}" -gt 0 ] || { skip "$pr" "no checks reported"; continue; }
  failing=$(gh pr checks "$pr" --json name,bucket \
    --jq '.[] | select(.bucket == "fail" or .bucket == "cancel" or .bucket == "pending") | .name')
  if [ -n "$failing" ]; then
    skip "$pr" "checks not green:
$failing"
    continue
  fi

  if [ -n "$missing" ]; then
    skip "$pr" "checks not reported after 10 minutes:
$missing"
    continue
  fi

  gh pr merge "$pr" --squash --match-head-commit "$head" || { skip "$pr" "gh pr merge failed"; continue; }
  git push -q origin --delete "$branch" ||
    echo "#$pr: merged; the remote branch $branch was not deleted" >&2
  echo "#$pr: merged"
done
exit "$skipped"

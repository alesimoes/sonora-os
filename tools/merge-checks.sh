#!/bin/sh
# Decision helpers for tools/merge-when-green.sh, sourced and tested apart
# (tools/test-merge-checks.sh).
#
# The heavy CI jobs have `needs: changes`, so GitHub creates their check runs
# only after "Changed paths" finishes. A poll in that window sees only the
# cheap jobs, all green. A merge needs every heavy check present, passed or
# skipped (a job skipped by `if:` still reports one, state SKIPPED).
#
# Decisions use gh's `state`, never `bucket`: bucket folds NEUTRAL (and
# STALE) into "skipping", which would let a check that neither passed nor was
# skipped by `if:` count as fine. Only SUCCESS and SKIPPED are green.

# Since ADR-0061 the PR gate is the micro build; "Gate" needs changes,
# micro and micro-rtsan and fails when any of them failed or was cancelled,
# so its presence and success stand for them. The nine full-build job names
# listed here before moved to the nightly and never report on a PR, which
# made every code PR "skipped: heavy checks not reported" (2026-11-07).
HEAVY_CHECKS="Gate"

# stdin: "name<TAB>state" lines. Prints each heavy check that is absent or
# not SUCCESS/SKIPPED; prints nothing when all are present and fine.
missing_heavy_checks() {
  checks=$(cat)
  printf '%s\n' "$HEAVY_CHECKS" | while IFS= read -r want; do
    printf '%s\n' "$checks" | awk -F'\t' -v w="$want" \
      '$1 == w && ($2 == "SUCCESS" || $2 == "SKIPPED") { found = 1 } END { exit !found }' ||
      printf '%s\n' "$want"
  done
}

# The final decision for one PR. Every input comes from one snapshot of the
# checks taken between two reads of the head commit, and the head must be the
# same both times: a push in between could otherwise pair an earlier head's
# full set of heavy checks with a new head that has only the cheap ones so
# far. A push after the second read is refused by the merge itself
# (--match-head-commit). Anything gh does not answer cleanly refuses.
# Prints the head SHA and returns 0 when the PR may merge; prints the reason
# and returns 1 otherwise.
merge_gate() {
  pr=$1
  gate_head=$(gh pr view "$pr" --json headRefOid --jq .headRefOid 2>/dev/null) ||
    gate_head=""
  [ -n "$gate_head" ] || { echo "could not read the head commit"; return 1; }
  # gh pr checks exits non-zero while a check is pending or failed, so its
  # status says nothing about the read; an empty answer is what refuses.
  gate_checks=$(gh pr checks "$pr" --json name,state \
    --jq '.[] | [.name, .state] | @tsv' 2>/dev/null)
  gate_again=$(gh pr view "$pr" --json headRefOid --jq .headRefOid 2>/dev/null) ||
    gate_again=""
  if [ "$gate_again" != "$gate_head" ]; then
    echo "the head moved while its checks were read ($gate_head -> ${gate_again:-unreadable})"
    return 1
  fi
  [ -n "$gate_checks" ] || { echo "no checks reported"; return 1; }
  gate_failing=$(printf '%s\n' "$gate_checks" |
    awk -F'\t' '$2 != "SUCCESS" && $2 != "SKIPPED" { print $1 }')
  if [ -n "$gate_failing" ]; then
    printf 'checks not green:\n%s\n' "$gate_failing"
    return 1
  fi
  gate_missing=$(printf '%s\n' "$gate_checks" | missing_heavy_checks)
  if [ -n "$gate_missing" ]; then
    printf 'heavy checks not reported:\n%s\n' "$gate_missing"
    return 1
  fi
  echo "$gate_head"
}

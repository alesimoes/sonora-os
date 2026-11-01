#!/bin/sh
# Dry run of the merge decision against fake `gh pr checks` output.
set -u
# shellcheck source=/dev/null
. "$(dirname "$0")/merge-checks.sh"
fail=0
expect() {  # label, expected count of missing heavy checks, input
  n=$(printf '%s\n' "$3" | missing_heavy_checks | grep -c .)
  [ "$n" = "$2" ] || { echo "FAIL $1: $n missing, expected $2"; fail=1; }
}
tab=$(printf '\t')
cheap="Changed paths${tab}pass
Executable bits${tab}pass
Progress catalogue${tab}pass
Shell scripts${tab}pass"
all=$(printf '%s\n' "$HEAVY_CHECKS" | awk '{ print $0 "\tpass" }')
skipped=$(printf '%s\n' "$HEAVY_CHECKS" | awk '{ print $0 "\tskipping" }')
no_engine=$(printf '%s\n' "$all" | grep -v '^Engine build')

expect "only cheap checks (before dependents exist)" 9 "$cheap"
expect "all heavy pass" 0 "$cheap
$all"
expect "doc-only: all heavy skipping" 0 "$cheap
$skipped"
expect "engine absent" 1 "$cheap
$no_engine"
expect "engine pending" 1 "$cheap
$no_engine
Engine build and tests${tab}pending"
expect "engine failed" 1 "$cheap
$no_engine
Engine build and tests${tab}fail"
[ "$fail" = 0 ] && echo "ok"
exit "$fail"

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
cheap="Changed paths${tab}SUCCESS
Executable bits${tab}SUCCESS
Progress catalogue${tab}SUCCESS
Shell scripts${tab}SUCCESS"
all=$(printf '%s\n' "$HEAVY_CHECKS" | awk '{ print $0 "\tSUCCESS" }')
skipped=$(printf '%s\n' "$HEAVY_CHECKS" | awk '{ print $0 "\tSKIPPED" }')
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
Engine build and tests${tab}IN_PROGRESS"
expect "engine failed" 1 "$cheap
$no_engine
Engine build and tests${tab}FAILURE"

# Any state but SUCCESS or SKIPPED in a heavy slot is missing. NEUTRAL is the
# one gh's bucket calls "skipping"; it must not pass.
for st in FAILURE CANCELLED ERROR TIMED_OUT STARTUP_FAILURE ACTION_REQUIRED           NEUTRAL STALE IN_PROGRESS QUEUED PENDING WAITING EXPECTED success pass skipping ""; do
  expect "heavy check in state '$st'" 1 "$cheap
$no_engine
Engine build and tests${tab}$st"
done

# merge-when-green.sh end to end, against a fake gh that can "push" a new head
# right after its Nth read of the check list. GitHub's own --match-head-commit
# refusal is faked too, so only a pairing of checks and head that the script
# itself got wrong reaches the merge log.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bin"
cat >"$work/bin/gh" <<'FAKE'
#!/bin/sh
d=$FAKE_GH
case "$*" in
  "pr view"*headRefOid*) [ -s "$d/head" ] || exit 1; cat "$d/head" ;;
  "pr view"*) echo "MERGEABLE CLEAN topic" ;;
  "pr checks"*--watch*) ;;
  "pr checks"*@tsv*)
    cat "$d/checks"
    n=$(($(cat "$d/reads") + 1)); echo "$n" >"$d/reads"
    if [ "$n" = "$(cat "$d/push_at")" ]; then
      echo B >"$d/head"; cp "$d/checks_b" "$d/checks"
    fi ;;
  "pr checks"*length*) grep -c . "$d/checks" ;;
  "pr checks"*select*)
    awk -F'\t' '$2 == "fail" || $2 == "cancel" || $2 == "pending" { print $1 }' "$d/checks" ;;
  "pr checks"*bucket*) cut -f2 "$d/checks" ;;
  "pr merge"*)
    want=$(echo "$*" | sed 's/.*--match-head-commit //')
    [ "$want" = "$(cat "$d/head")" ] || exit 1
    echo "merged $want" >>"$d/log" ;;
  *) echo "fake gh: unexpected: $*" >&2; exit 1 ;;
esac
FAKE
printf '#!/bin/sh\n' >"$work/bin/git"
printf '#!/bin/sh\n' >"$work/bin/sleep"
chmod +x "$work/bin/gh" "$work/bin/git" "$work/bin/sleep"

run() {  # label, head, push_at (0 = never), expected merge log
  s="$work/$1"
  mkdir "$s"
  printf '%s\n' "$2" >"$s/head"
  printf '%s\n%s\n' "$cheap" "$all" >"$s/checks"
  printf '%s\n' "$cheap" >"$s/checks_b"
  echo 0 >"$s/reads"
  echo "$3" >"$s/push_at"
  : >"$s/log"
  FAKE_GH=$s PATH="$work/bin:$PATH" sh "$(dirname "$0")/merge-when-green.sh" 7 \
    >"$s/out" 2>&1
  got=$(cat "$s/log")
  [ "$got" = "$4" ] || {
    echo "FAIL $1: merge log '$got', expected '$4'"; sed 's/^/  /' "$s/out"; fail=1; }
}
run "all green, no push" A 0 "merged A"
# The race: the wait loop sees head A's full set, then B lands with only the
# cheap checks registered.
run "push after the wait loop's snapshot" A 1 ""
run "push between the gate's reads" A 2 ""
run "head read empty" "" 0 ""

# The gate itself. Any single check in a non-green state refuses the merge,
# cheap or heavy.
gate_case() {  # label, state to put on the check named by $3, check, expected
  s="$work/g$1"
  mkdir "$s"
  echo A >"$s/head"
  printf '%s
%s
' "$cheap" "$all" | awk -F'	' -v n="$3" -v st="$2"     'BEGIN { OFS = "	" } $1 == n { $2 = st } { print }' >"$s/checks"
  echo 0 >"$s/reads"; echo 0 >"$s/push_at"; : >"$s/log"
  FAKE_GH=$s PATH="$work/bin:$PATH" sh "$(dirname "$0")/merge-when-green.sh" 7     >"$s/out" 2>&1
  got=$(cat "$s/log")
  [ "$got" = "$4" ] || { echo "FAIL gate $1: merge log '$got', expected '$4'"; fail=1; }
}
i=0
for st in FAILURE CANCELLED ERROR TIMED_OUT NEUTRAL STALE IN_PROGRESS QUEUED PENDING ACTION_REQUIRED; do
  for name in "Shell scripts" "Engine build and tests"; do
    i=$((i + 1))
    gate_case "$i-$st" "$st" "$name" ""
  done
done
gate_case ok SKIPPED "Engine build and tests" "merged A"

# Without the second head read the push-between-reads case merges. Compare
# the head with itself instead and require that the case then goes through:
# proof the test above fails when the read is removed.
here=$(dirname "$0")
# shellcheck disable=SC2016 # the pattern is literal text
sed 's/"\$gate_again" != "\$gate_head"/"$gate_again" != "$gate_again"/'   "$here/merge-checks.sh" >"$work/no-second-read.sh"
if cmp -s "$work/no-second-read.sh" "$here/merge-checks.sh"; then
  echo "FAIL mutation: could not remove the second head read"; fail=1
else
  gate_push() {  # sourced file, expected "refuse" or "merge"
    s="$work/gp"; rm -rf "$s"; mkdir "$s"
    echo A >"$s/head"; printf '%s
%s
' "$cheap" "$all" >"$s/checks"
    printf '%s
' "$cheap" >"$s/checks_b"
    echo 0 >"$s/reads"; echo 1 >"$s/push_at"
    # shellcheck disable=SC1090
    (. "$1"; FAKE_GH=$s PATH="$work/bin:$PATH" merge_gate 7 >/dev/null) && got=merge || got=refuse
    [ "$got" = "$2" ] || { echo "FAIL $1: push during the checks read: $got, expected $2"; fail=1; }
  }
  gate_push "$here/merge-checks.sh" refuse
  gate_push "$work/no-second-read.sh" merge
fi

[ "$fail" = 0 ] && echo "ok"
exit "$fail"

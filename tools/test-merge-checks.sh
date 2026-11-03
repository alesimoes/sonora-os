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

[ "$fail" = 0 ] && echo "ok"
exit "$fail"

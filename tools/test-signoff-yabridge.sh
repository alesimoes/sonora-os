#!/usr/bin/env bash
# Tests for the sign-off evaluator and the no-Wine exit. No Wine needed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/lib/yabridge-signoff.sh
source "$here/lib/yabridge-signoff.sh"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
fails=0
check() {
  if [[ "$2" == 1 ]]; then echo "ok - $1"; else
    echo "not ok - $1"
    fails=1
  fi
}
has() { if grep -qF -- "$2" "$1"; then echo 1; else echo 0; fi; }
is() { if [[ "$1" == "$2" ]]; then echo 1; else echo 0; fi; }

# Fixtures: 1000 latencies of 100..1099 us; a second set with a slow tail.
seq 100 1099 >"$W/good.lat"
{
  seq 100 1099
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do echo 2400; done
} >"$W/slow.lat"
printf 'CLS RTPRIO COMMAND\nTS      - bash\nFF     70 wineserver\nFF     80 yabridge-host.e\n' >"$W/ps-rt.txt"
printf 'CLS RTPRIO COMMAND\nTS      - wineserver\nTS      - yabridge-host.e\n' >"$W/ps-none.txt"
printf '  driver: 1500 callbacks\n  the host died at block 210 (SIGSEGV), running instance 1 (x)\n' >"$W/crash-ok.log"
printf '  the host stopped answering; out of the plan at block 230 after 3 misses\n' >"$W/crash-quiet.log"
printf '  driver: 1500 callbacks\n' >"$W/crash-none.log"

# eval_run LAT PS CRASH RC DROPPED: report in $W/out, returns the verdict.
eval_run() {
  local a b c d n
  read -r a b c d n <<<"$(yb_stats "$1")"
  yb_evaluate "$a" "$b" "$c" "$d" "${5:-0}" "$2" "$3" "$4" >"$W/out"
}

read -r p50 p99 _ max n <<<"$(yb_stats "$W/good.lat")"
check "percentiles of 100..1099" "$(is "$p50 $p99 $max $n" "599.0 1089.0 1099.0 1000")"

rc=0
eval_run "$W/good.lat" "$W/ps-rt.txt" "$W/crash-ok.log" 0 || rc=$?
check "passing run exits 0" "$(is $rc 0)"
check "passing run says PASS" "$(has "$W/out" 'SIGN-OFF: PASS')"

rc=0
eval_run "$W/good.lat" "$W/ps-rt.txt" "$W/crash-quiet.log" 0 || rc=$?
check "host gone silent also passes" "$(is $rc 0)"

rc=0
eval_run "$W/slow.lat" "$W/ps-rt.txt" "$W/crash-ok.log" 0 || rc=$?
check "p99.9 over the deadline exits 1" "$(is $rc 1)"
check "names the p99.9 criterion" "$(has "$W/out" 'FAIL  p99.9 inside the deadline')"

rc=0
eval_run "$W/good.lat" "$W/ps-none.txt" "$W/crash-ok.log" 0 || rc=$?
check "no RT rows exits 1" "$(is $rc 1)"
check "names the RT criterion" "$(has "$W/out" 'FAIL  Wine threads have RT priority')"

rc=0
eval_run "$W/good.lat" "$W/ps-rt.txt" "$W/crash-none.log" 0 || rc=$?
check "crash scenario silent (neither died nor stopped) exits 1" "$(is $rc 1)"
check "names the crash criterion" "$(has "$W/out" 'FAIL  crash_bridged')"

rc=0
eval_run "$W/good.lat" "$W/ps-rt.txt" "$W/crash-ok.log" 1 || rc=$?
check "crash driver failure exits 1" "$(is $rc 1)"

rc=0
eval_run "$W/good.lat" "$W/ps-rt.txt" "$W/crash-ok.log" 0 2 || rc=$?
check "a dropped block exits 1" "$(is $rc 1)"

printf '  round trip us: p50 161.0  p99 598.2  p99.9 755.0  max 793.4  (n=3000)\n' >"$W/rtt.log"
printf '  dropped 2 of 3000 (0.07 %%), of which 0 noticed\n' >>"$W/rtt.log"
check "parses plugin_driver's rtt lines" "$(is "$(yb_parse_rtt "$W/rtt.log")" "161.0 598.2 755.0 793.4 2")"

# Without Wine on PATH the script exits 77 and names what is missing.
rc=0
PATH=/nonexistent /bin/bash "$here/signoff-yabridge.sh" --host /x --driver /y --plugin /z >"$W/skip.out" 2>&1 || rc=$?
check "no Wine exits 77" "$(is $rc 77)"
check "says what is missing" "$(has "$W/skip.out" 'not installed or not on PATH: wine')"

exit "$fails"

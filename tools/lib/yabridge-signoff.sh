#!/usr/bin/env bash
# Criteria evaluator for tools/signoff-yabridge.sh. Sourced, no side effects,
# no Wine: tools/test-signoff-yabridge.sh feeds it fixtures. Times are
# microseconds. The limits are the engine's: a 128-frame period at 48 kHz and
# the 2 ms deadline the plugin boundary drops a block at (ADR-0017).

YB_PERIOD_US="${YB_PERIOD_US:-2667}"
YB_DEADLINE_US="${YB_DEADLINE_US:-2000}"

# yb_stats FILE: one latency in us per line -> "p50 p99 p99.9 max n".
# Nearest-rank percentiles.
yb_stats() {
  sort -g "$1" | awk '
    NF { v[n++] = $1 }
    function pct(p,   i) { i = int(p / 100 * n + 0.9999999); if (i < 1) i = 1; if (i > n) i = n; return v[i - 1] }
    END { if (!n) { print "0 0 0 0 0"; exit } printf "%.1f %.1f %.1f %.1f %d\n", pct(50), pct(99), pct(99.9), v[n - 1], n }'
}

# yb_parse_rtt LOG: plugin_driver's rtt output -> "p50 p99 p99.9 max dropped".
yb_parse_rtt() {
  awk '
    /round trip us:/ {
      for (i = 1; i <= NF; i++) {
        if ($i == "p50") a = $(i + 1)
        if ($i == "p99") b = $(i + 1)
        if ($i == "p99.9") c = $(i + 1)
        if ($i == "max") d = $(i + 1)
      }
      ok = 1
    }
    /^ *dropped [0-9]+ of/ { e = $2; k = 1 }
    END { if (ok && k) print a, b, c, d, e; else exit 1 }' "$1"
}

# yb_wine_rt_count FILE: threads in `ps -eLo cls,rtprio,comm` rows that are
# Wine's and run SCHED_FIFO or SCHED_RR with a priority above 0.
yb_wine_rt_count() {
  awk '$1 ~ /^(FF|RR)$/ && $2 ~ /^[0-9]+$/ && $2 > 0 && tolower($3) ~ /wine|yabridge|\.exe/ { n++ } END { print n + 0 }' "$1"
}

# yb_crash_outcome LOG: how the crash_bridged scenario ended.
yb_crash_outcome() {
  if grep -q 'the host died at block' "$1"; then
    echo died
  elif grep -q 'the host stopped answering' "$1"; then
    echo silent
  else
    echo none
  fi
}

# yb_ok CONDITION...: prints 1 when the command succeeds, else 0.
yb_ok() { if "$@"; then echo 1; else echo 0; fi; }
yb_le() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a <= b) }'; }
yb_eq() { [[ "$1" == "$2" ]]; }
yb_gt0() { [[ "$1" -gt 0 ]]; }

# yb_evaluate P50 P99 P999 MAX DROPPED PS_FILE CRASH_LOG CRASH_RC
# Prints the report; returns 0 only when every criterion holds.
yb_evaluate() {
  local p50="$1" p99="$2" p999="$3" max="$4" dropped="$5" psf="$6" crashlog="$7" crashrc="$8"
  local failed=0 rt outcome crash_ok=0
  crit() { # NAME OK DETAIL
    if [[ "$2" == 1 ]]; then
      echo "  PASS  $1: $3"
    else
      echo "  FAIL  $1: $3"
      failed=1
    fi
  }
  echo "yabridge sign-off report"
  echo "  round trip us: p50 $p50  p99 $p99  p99.9 $p999  max $max"
  echo "  period $YB_PERIOD_US us, deadline $YB_DEADLINE_US us, dropped $dropped"
  crit "p99.9 inside the deadline" "$(yb_ok yb_le "$p999" "$YB_DEADLINE_US")" "$p999 us against $YB_DEADLINE_US us"
  crit "max inside the period" "$(yb_ok yb_le "$max" "$YB_PERIOD_US")" "$max us against $YB_PERIOD_US us"
  crit "no block dropped" "$(yb_ok yb_eq "$dropped" 0)" "$dropped dropped"
  rt="$(yb_wine_rt_count "$psf")"
  crit "Wine threads have RT priority" "$(yb_ok yb_gt0 "$rt")" \
    "$rt SCHED_FIFO/RR thread(s) with rtprio > 0 in ps -eLo cls,rtprio,comm"
  outcome="$(yb_crash_outcome "$crashlog")"
  if [[ "$crashrc" == 0 && "$outcome" != none ]]; then crash_ok=1; fi
  crit "crash_bridged: host died or went silent, callbacks ran" "$crash_ok" \
    "outcome $outcome, driver exit $crashrc"
  if [[ "$failed" == 0 ]]; then echo "SIGN-OFF: PASS"; else echo "SIGN-OFF: FAIL"; fi
  return "$failed"
}

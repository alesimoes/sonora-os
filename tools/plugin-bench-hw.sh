#!/usr/bin/env bash
# Hardware rerun of the 18 x 4 plugin benchmark (ADR-0135): sonora-graph-bench
# on the ALSA backend, 3 runs, failed at once on the first xrun or overrun.
#
#   tools/plugin-bench-hw.sh --buffer N [--seconds S] [--warmup S] [--runs N]
#                            [--out DIR] [--device NAME] [--rate R]
#   tools/plugin-bench-hw.sh --self-test
#
# Run it on the installed system with the RT kernel, never on the live stick.
# Exit status: 0 PASS, 1 FAIL, 2 setup refused (nothing was started).
# --self-test: fake backend, proves a PASS and an injected-xrun FAIL, skips
# the hardware preflight and says so; it is the only part CI may run.
#
# Test hooks: SONORA_BENCH (bench binary), SONORA_BENCH_ARGS (words replacing
# the backend arguments), SONORA_XRUN_CMD (prints the xrun and overrun count
# of the control state), SONORA_CAPTURE_CHANNELS (channel count override).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BENCH="${SONORA_BENCH:-sonora-graph-bench}"
BUFFER=""
SECONDS_TOTAL=600
WARMUP=60
RUNS=3
OUT_DIR="$ROOT/brain/research"
DEVICE=""
RATE=48000
TRACKS=18
SELF_TEST=0
PREFLIGHT_HW=1

usage() { sed -n '2,15p' "$0"; }
refuse() { echo "REFUSED: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    --buffer) BUFFER="${2:-}"; shift 2 ;;
    --seconds) SECONDS_TOTAL="${2:-}"; shift 2 ;;
    --warmup) WARMUP="${2:-}"; shift 2 ;;
    --runs) RUNS="${2:-}"; shift 2 ;;
    --out) OUT_DIR="${2:-}"; shift 2 ;;
    --device) DEVICE="${2:-}"; shift 2 ;;
    --rate) RATE="${2:-}"; shift 2 ;;
    --self-test) SELF_TEST=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# ---- self-test ------------------------------------------------------------
if [ "$SELF_TEST" = 1 ]; then
  T="$(mktemp -d)"
  trap 'rm -rf "$T"' EXIT
  fails=0
  common=(--buffer 128 --seconds 2 --warmup 0 --runs 1 --out "$T/out")
  echo "self-test 1/2: clean run must PASS"
  SONORA_BENCH_NO_PREFLIGHT=1 "$0" "${common[@]}" >"$T/pass.log" 2>&1
  rc=$?
  if [ "$rc" != 0 ]; then echo "FAIL: clean run exited $rc, want 0" >&2; tail -15 "$T/pass.log" >&2; fails=1; fi
  echo "self-test 2/2: an injected xrun must FAIL at once"
  printf '#!/bin/sh\necho 1\n' >"$T/xr"; chmod +x "$T/xr"
  SONORA_BENCH_NO_PREFLIGHT=1 SONORA_XRUN_CMD="$T/xr" "$0" "${common[@]}" >"$T/fail.log" 2>&1
  rc=$?
  if [ "$rc" != 1 ]; then echo "FAIL: injected xrun exited $rc, want 1" >&2; tail -15 "$T/fail.log" >&2; fails=1; fi
  grep -q 'first xrun' "$T/fail.log" || { echo "FAIL: no early-abort line in the FAIL run" >&2; fails=1; }
  if [ "$fails" = 0 ]; then echo "self-test: ok (hardware preflight skipped)"; else echo "self-test: FAILED" >&2; fi
  exit "$fails"
fi

[ -n "$BUFFER" ] || refuse "--buffer N is required (the quantum is chosen explicitly)"
case "$BUFFER$SECONDS_TOTAL$WARMUP$RUNS$RATE" in ""|*[!0-9]*) refuse "--buffer, --seconds, --warmup, --runs, --rate must be whole numbers" ;; esac
[ "$BUFFER" -gt 0 ] && [ "$RUNS" -gt 0 ] || refuse "--buffer and --runs must be positive"
[ -n "${SONORA_BENCH_NO_PREFLIGHT:-}" ] && PREFLIGHT_HW=0

# ---- preflight: refuse before starting --------------------------------------
KERNEL="$(uname -r) $(uname -v)"
GOVERNOR="not checked"
LIMITS="not checked"
CHANNELS="not checked"
if [ "$PREFLIGHT_HW" = 1 ]; then
  case "$(uname -v)" in
    *PREEMPT_RT*) ;;
    *) [ -e /sys/kernel/realtime ] || refuse "no RT kernel (uname -v has no PREEMPT_RT, no /sys/kernel/realtime)" ;;
  esac
  GOVS="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u)"
  [ "$GOVS" = performance ] || refuse "CPU governor is '${GOVS:-unavailable}', want performance on every CPU"
  GOVERNOR=performance
  [ "$(ulimit -l)" = unlimited ] || refuse "memlock limit is $(ulimit -l), want unlimited"
  RTP="$(ulimit -r)"
  { [ "$RTP" = unlimited ] || [ "$RTP" -ge 50 ]; } || refuse "RT priority limit is $RTP, want >= 50"
  LIMITS="memlock unlimited, rtprio $RTP"
  if [ -n "${SONORA_CAPTURE_CHANNELS:-}" ]; then CH="$SONORA_CAPTURE_CHANNELS"
  else
    CARD_NO="$(arecord -l 2>/dev/null | grep -i -E "${DEVICE:-UMC|behringer}" | sed -n 's/^card \([0-9]*\):.*/\1/p' | head -1)"
    [ -n "$CARD_NO" ] || refuse "no capture card found in arecord -l"
    CH="$(arecord -D "hw:$CARD_NO,0" --dump-hw-params -d 1 /dev/null 2>&1 </dev/null | grep -E '^CHANNELS:' | grep -o '[0-9]\+' | sort -n | tail -1)"
  fi
  [ "${CH:-0}" -ge "$TRACKS" ] || refuse "card reports ${CH:-0} capture channels, want $TRACKS"
  CHANNELS="$CH"
fi
command -v "$BENCH" >/dev/null || [ -x "$BENCH" ] || refuse "bench '$BENCH' not found (set SONORA_BENCH)"
mkdir -p "$OUT_DIR" || refuse "cannot create $OUT_DIR"

# ---- runs ------------------------------------------------------------------
if [ "$PREFLIGHT_HW" = 1 ]; then
  BACKEND_ARGS=(--backend alsa --alsa-period "$BUFFER")
  [ -z "$DEVICE" ] || BACKEND_ARGS+=(--device "$DEVICE")
else
  BACKEND_ARGS=(--fake)
fi
# shellcheck disable=SC2206
[ -z "${SONORA_BENCH_ARGS:-}" ] || BACKEND_ARGS=(${SONORA_BENCH_ARGS})
CALLBACKS=$(((SECONDS_TOTAL + WARMUP) * RATE / BUFFER))
REPORT="$OUT_DIR/plugin-benchmark-umc1820.md"
[ "$PREFLIGHT_HW" = 1 ] || REPORT="$OUT_DIR/plugin-benchmark-selftest.md"
ROWS=()
FAILED=0
ABORT=""
LIMIT_US=$((BUFFER * 1000000 / RATE))

field() { sed -n "s/.* $1=\\([0-9.]*\\).*/\\1/p" <<<"$2"; }
xruns_now() { [ -n "${SONORA_XRUN_CMD:-}" ] && $SONORA_XRUN_CMD 2>/dev/null | head -1; }
nonzero() { case "${1:-0}" in 0|"") return 1 ;; *[!0-9]*) return 1 ;; *) return 0 ;; esac; }

for run in $(seq 1 "$RUNS"); do
  OUTF="$OUT_DIR/.plugin-bench-run$run.out"
  "$BENCH" "${BACKEND_ARGS[@]}" --quantum "$BUFFER" --rate "$RATE" --callbacks "$CALLBACKS" >"$OUTF" 2>&1 &
  BPID=$!
  trap 'kill "$BPID" 2>/dev/null' EXIT
  echo "run $run/$RUNS: pid $BPID, quantum $BUFFER, $CALLBACKS callbacks"
  XR=""
  while kill -0 "$BPID" 2>/dev/null; do
    XR="$(xruns_now)"
    if nonzero "$XR"; then
      ABORT="first xrun/overrun in the control state during run $run: count=$XR"
      echo "FAIL: $ABORT; stopping the bench"
      kill "$BPID" 2>/dev/null
      break
    fi
    sleep 1
  done
  wait "$BPID" 2>/dev/null
  RC=$?
  trap - EXIT
  if [ -n "$ABORT" ]; then ROWS+=("| $run | - | - | - | $XR | aborted |"); FAILED=1; break; fi
  XR="$(xruns_now)"
  nonzero "$XR" && ABORT="first xrun/overrun in the control state after run $run: count=$XR"
  RES="$(grep '^RESULT' "$OUTF" | tail -1)"
  MAX="$(field max_us "$RES")"; P99="$(field p99_us "$RES")"; MISS="$(field misses "$RES")"
  DROPS="$(field drops "$RES")"
  OK=1
  { [ -n "$RES" ] && [ "$RC" = 0 ]; } || OK=0
  { [ "${MISS:-x}" = 0 ] && [ "${DROPS:-x}" = 0 ]; } || OK=0
  [ -z "$ABORT" ] || OK=0
  awk -v m="${MAX:-0}" -v l="$LIMIT_US" 'BEGIN {exit !(m < l)}' || OK=0
  if [ "$OK" = 1 ]; then V=PASS; else V="FAIL (exit $RC)"; fi
  ROWS+=("| $run | ${P99:-?} | ${MAX:-?} | ${MISS:-?} / ${DROPS:-?} | ${XR:-na} | $V |")
  [ "$OK" = 1 ] || FAILED=1
  if [ -n "$ABORT" ]; then echo "FAIL: $ABORT"; break; fi
done

# ---- report ----------------------------------------------------------------
if [ "$FAILED" = 0 ]; then VERDICT=PASS; RC=0; else VERDICT=FAIL; RC=1; fi
{
  echo "# Plugin benchmark 18 x 4: $VERDICT"
  echo
  echo "- host: $(hostname 2>/dev/null || echo unknown), kernel \`$KERNEL\`"
  echo "- date: $(date -u +%Y-%m-%dT%H:%M:%SZ), quantum $BUFFER ($LIMIT_US us), $RATE Hz, warm-up ${WARMUP} s, measured ${SECONDS_TOTAL} s, $RUNS run(s)"
  echo "- preflight: governor $GOVERNOR; limits $LIMITS; capture channels $CHANNELS"
  echo "- criterion: zero xruns, zero overruns, worst process() under $LIMIT_US us on every run"
  echo
  echo "| run | p99 us | max us | misses / drops | xruns | verdict |"
  echo "|---|---|---|---|---|---|"
  printf '%s\n' "${ROWS[@]}"
  echo
  echo "## Not measured"
  echo
  [ "$PREFLIGHT_HW" = 1 ] || echo "- hardware preflight skipped and fake backend used (self-test): not hardware evidence"
  [ -n "${SONORA_XRUN_CMD:-}" ] || echo "- no control-state xrun hook (SONORA_XRUN_CMD): xruns come from the bench's RESULT line only"
} >"$REPORT"
rm -f "$OUT_DIR"/.plugin-bench-run*.out
echo "$VERDICT; report: $REPORT"
exit "$RC"

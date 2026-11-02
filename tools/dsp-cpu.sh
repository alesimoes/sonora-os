#!/usr/bin/env bash
# The CPU figures ADR-0011 §3.5 defines, for every native processor: p50,
# p99.9 and max per block at quantum 128, 48 kHz, stereo, single thread,
# over a 60 s render of the harness signals, against the processor's class
# bar in its quality.json.
#
# Not a ctest. A 60 s render per configuration, five times over, is minutes,
# and a shared CI runner says nothing about the reference machine. ctest
# keeps the quick per-block check (denormals: silence against active, side
# by side). The nightly workflow runs this and keeps the reports; the
# figures that count are the ones from the reference machine, recorded in
# brain/research/native-dsp-n0-measurements.md with the CPU model.
#
# Usage, on Linux, after a Release build of the root (tools/build-engine.sh):
#   tools/dsp-cpu.sh [--calibrated] [build-dir] [seconds] [processor...]
# build-dir defaults to build/ (build-container/ when run inside the
# sonora-build image from Windows); seconds defaults to 60.
#
# Without --calibrated the class bars are absolute: the run that counts
# is on the reference machine. --calibrated is for a machine of unknown
# speed (the nightly's shared runner): each bar is multiplied by this
# machine's cost of a frozen reference kernel over the reference
# machine's (plugins/native/measure/src/calibrate.cpp), so the bar moves
# with the runner and a red means the processor got slower, not the
# runner. Where quality.json records cpu.p999_kernels, the p99.9 in
# kernels is also held to it x 1.15. The kernel is timed before and after
# the loads; over 5 % apart, the run is inconclusive (exit 3).
# brain/research/native-tube-n2.md, "The gate".
#
# Each line is the harness's own: "info" is a figure, "ok"/"FAIL" a figure
# against the class bar. "code path" is the per-block minimum over five
# identical renders; "single run" includes the host's scheduling.
# Reports: <build-dir>/dsp-reports-cpu/<processor>.json and .log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CALIBRATE=()
if [[ "${1:-}" == "--calibrated" ]]; then
  CALIBRATE=(--cpu-calibrate)
  shift
fi
BUILD="${1:-$ROOT/build}"
SECONDS_ARG="${2:-60}"
shift $(( $# > 2 ? 2 : $# ))
PROCESSORS=("$@")
if [[ ${#PROCESSORS[@]} -eq 0 ]]; then
  PROCESSORS=(eq compressor gate limiter delay chorus reverb tube opto fet tape)
fi
MEASURE="$BUILD/sonora-dsp-measure"
OUT="$BUILD/dsp-reports-cpu"

if [[ ! -x "$MEASURE" ]]; then
  echo "error: $MEASURE not found; build the root in Release first." >&2
  exit 2
fi
mkdir -p "$OUT"

# 0: every processor met its bars; 1: one failed; 3: none failed, but a
# calibrated run was inconclusive (the machine changed speed between the
# reference kernel's two timings, so the scaled bars could not be held).
status=0
inconclusive=0
for p in "${PROCESSORS[@]}"; do
  rc=0
  # ${CALIBRATE[@]+...}: an empty array under set -u, on any bash.
  "$MEASURE" --processor "$p" \
      --quality "$ROOT/plugins/native/$p/quality.json" \
      --golden "$ROOT/plugins/native/$p/tests/golden" \
      --report "$OUT" --cpu --cpu-seconds "$SECONDS_ARG" ${CALIBRATE[@]+"${CALIBRATE[@]}"} \
      > "$OUT/$p.log" 2>&1 || rc=$?
  if [[ $rc -eq 3 ]]; then
    inconclusive=1
  elif [[ $rc -ne 0 ]]; then
    status=1
  fi
  echo "== $p"
  grep -E "CPU per block|CPU reference kernel|CPU runner scale|INCONCLUSIVE|meets its bars|FAILS" "$OUT/$p.log" || true
done
if [[ $status -eq 0 && $inconclusive -eq 1 ]]; then
  verdict="INCONCLUSIVE: no bar failed, but a calibrated run could not be held (see $OUT)"
  status=3
elif [[ $status -eq 0 ]]; then
  verdict="every processor meets its bars"
else
  verdict="FAIL (see $OUT)"
fi
echo "dsp-cpu: $verdict, ${SECONDS_ARG} s renders, $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"
exit "$status"

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
#   tools/dsp-cpu.sh [build-dir] [seconds]
# build-dir defaults to build/ (build-container/ when run inside the
# sonora-build image from Windows); seconds defaults to 60.
#
# Each line is the harness's own: "info" is a figure, "ok"/"FAIL" a figure
# against the class bar. "code path" is the per-block minimum over five
# identical renders; "single run" includes the host's scheduling.
# Reports: <build-dir>/dsp-reports-cpu/<processor>.json and .log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
SECONDS_ARG="${2:-60}"
MEASURE="$BUILD/sonora-dsp-measure"
OUT="$BUILD/dsp-reports-cpu"

if [[ ! -x "$MEASURE" ]]; then
  echo "error: $MEASURE not found; build the root in Release first." >&2
  exit 2
fi
mkdir -p "$OUT"

status=0
for p in eq compressor gate limiter delay chorus; do
  if ! "$MEASURE" --processor "$p" \
      --quality "$ROOT/plugins/native/$p/quality.json" \
      --golden "$ROOT/plugins/native/$p/tests/golden" \
      --report "$OUT" --cpu --cpu-seconds "$SECONDS_ARG" > "$OUT/$p.log" 2>&1; then
    status=1
  fi
  echo "== $p"
  grep -E "CPU per block|meets its bars|FAILS" "$OUT/$p.log" || true
done
echo "dsp-cpu: $( [[ $status -eq 0 ]] && echo "every processor meets its bars" || echo "FAIL (see $OUT)" ), ${SECONDS_ARG} s renders, $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"
exit "$status"

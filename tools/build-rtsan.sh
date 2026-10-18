#!/usr/bin/env bash
# Build the tree with clang-22 and -fsanitize=realtime and run its tests
# under RealtimeSanitizer (ADR-0007, first 2026-10-12 update). Every
# SONORA_RT function is [[clang::nonblocking]] in this build, so a test that
# drives one into malloc, a mutex or a blocking call fails with RTSan's
# report. Runs natively where clang-22 is installed, in a container
# (tools/Dockerfile.rtsan) elsewhere. CI's rtsan job runs it. In the
# container, tools/rtsan-jack.sh then runs the JACK backend against live
# servers; SONORA_RTSAN_LIVE=0 skips that, =1 asks for it natively.
#
#   tools/build-rtsan.sh [ctest args...]    e.g. -R '^plugin_client'
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-rtsan:trixie
CLANG_VERSION=22

host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    (cd "$1" && pwd -W)
  else
    printf '%s' "$1"
  fi
}

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  export MSYS2_ARG_CONV_EXCL='*'
fi

# Inside the container or on a host that has the toolchain.
native_build() {
  local build="$1"
  shift
  cmake -S "$ROOT" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER="clang++-$CLANG_VERSION" \
    -DSONORA_RTSAN=ON
  cmake --build "$build" -j"$(nproc)"
  # halt_on_error: a report is a failure, never a line nobody reads.
  # tools/rtsan.supp is for audited exceptions and holds none: the one there
  # is, wake_all's futex wake (ADR-0012 A1/A3), is a scope in its source.
  # The symbolizer turns the report's addresses into our function names.
  RTSAN_OPTIONS="halt_on_error=true:suppressions=$ROOT/tools/rtsan.supp" \
  RTSAN_SYMBOLIZER_PATH="/usr/bin/llvm-symbolizer-$CLANG_VERSION" \
  SONORA_DSP_QUICK=1 \
    ctest --test-dir "$build" --output-on-failure -j"$(nproc)" "$@"
  # A test can pass with a report in its output: one whose child process
  # RTSan halts while the test expects that child to die (plugin_host_deadline
  # passed with sonora-plugin-host aborted on its plugin's nanosleep). So any
  # report in any test's output fails the run, except in the tests labelled
  # rtsan-report, whose pass condition is the report.
  # Every test's full output is in LastTest.log, which the next ctest call
  # overwrites.
  cp "$build/Testing/Temporary/LastTest.log" "$build/rtsan-ctest.log"
  local expected reported
  expected="$(ctest --test-dir "$build" -N -L rtsan-report | sed -n 's/^ *Test *#[0-9]*: //p')"
  reported="$(awk '/^[0-9]+\/[0-9]+ Testing: / { name = $3 }
                   /ERROR: RealtimeSanitizer/ { print name }' \
                "$build/rtsan-ctest.log" | sort -u)"
  local hidden=0
  for t in $reported; do
    if ! grep -qx "$t" <<<"$expected"; then
      echo "FAIL: $t passed with a RealtimeSanitizer report in its output:" >&2
      awk -v t="$t" '/^[0-9]+\/[0-9]+ Testing: / { on = ($3 == t) }
                     on && /RealtimeSanitizer|real-time|#[0-9]+ /' \
        "$build/rtsan-ctest.log" | head -30 >&2
      hidden=1
    fi
  done
  [[ "$hidden" == "0" ]] || return 1
  # JackBackend's callbacks against jackd2 and PipeWire, which ctest has no
  # server for. It starts servers and writes to /etc, so only in the
  # container this script starts (SONORA_RTSAN_CONTAINER), or where asked.
  if [[ "${SONORA_RTSAN_LIVE:-${SONORA_RTSAN_CONTAINER:-0}}" == "1" ]]; then
    bash "$ROOT/tools/rtsan-jack.sh" "$build"
  fi
}

if [[ "${1:-}" == "--native" ]]; then
  shift
  native_build "$ROOT/build-rtsan" "$@"
  exit 0
fi

if [[ "$(uname -s)" == "Linux" ]] && command -v "clang++-$CLANG_VERSION" >/dev/null && \
   command -v cmake >/dev/null && pkg-config --exists jack 2>/dev/null; then
  native_build "$ROOT/build-rtsan" "$@"
  exit 0
fi

echo "Building in a container (no clang++-$CLANG_VERSION with JACK headers found)."
command -v docker >/dev/null || {
  echo "error: docker is not installed, and this host cannot build natively." >&2
  exit 1
}
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

if [[ "${SONORA_RTSAN_IMAGE_BUILT:-}" != "1" ]]; then
  docker build -t "$IMAGE" --label org.sonora=1 -f "$(host_path "$ROOT/tools")/Dockerfile.rtsan" \
    "$(host_path "$ROOT/tools")"
fi
# The build directory is container-specific; keep it apart from build-container.
# Unlimited memlock, as tools/build-engine.sh gives the same tests. jackd2
# keeps its shared memory in /dev/shm, more than Docker's 64 MB default.
# SYS_NICE and an rtprio limit: tools/rtsan-jack.sh runs jackd2 at
# SCHED_FIFO, as the appliance runs its server (PR #77 RT audit B1).
docker run --rm --ulimit memlock=-1:-1 --shm-size=512m \
  --cap-add=SYS_NICE --ulimit rtprio=95:95 -e SONORA_RTSAN_CONTAINER=1 \
  -e SONORA_RTSAN_LIVE -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" \
  bash /src/tools/build-rtsan.sh --native "$@"

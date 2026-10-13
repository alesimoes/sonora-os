#!/usr/bin/env bash
# Build the whole CMake root with lilv present, so plugins/host is built, and
# run ctest: the plugin host tests against real LV2 plugins from Debian, the
# in-tree misbehaving plugins, and every other module's tests.
#
#   tools/build-plugins.sh                 build + full ctest
#   tools/build-plugins.sh -L plugins      build + only the plugin host tests
#   tools/build-plugins.sh --measure       build + a longer round-trip run
#
# Always in a container (sonora-plugins:trixie), so the plugin set is the one
# the tests were written against. The container gets CAP_SYS_NICE and RT
# limits, as in brain/research/adr-0007-measurements.md, so the driver and the
# host can run at SCHED_FIFO. Every timing it prints is a CONTAINER figure: a
# shared host kernel without PREEMPT_RT. It is not an appliance figure.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-plugins:trixie

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

MEASURE=0
CTEST_ARGS=()
for arg in "$@"; do
  case "$arg" in
    --measure) MEASURE=1 ;;
    *) CTEST_ARGS+=("$arg") ;;
  esac
done

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

docker build -q --label org.sonora=1 -t "$IMAGE" \
  -f "$(host_path "$ROOT/tools")/Dockerfile.plugins" "$(host_path "$ROOT/tools")" >/dev/null

# The build directory is container-specific and ignored by git.
docker run --rm --label org.sonora=1 \
  --cap-add=SYS_NICE --ulimit rtprio=99 --ulimit memlock=-1 \
  -e MEASURE="$MEASURE" \
  -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -c '
  set -euo pipefail
  cmake -S . -B build-plugins -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build-plugins -j"$(nproc)"
  ctest --test-dir build-plugins --output-on-failure "$@"
  if [[ "$MEASURE" == 1 ]]; then
    d=build-plugins
    for chain in http://lv2plug.in/plugins/eg-amp \
                 http://gareus.org/oss/lv2/dpl#mono,http://lsp-plug.in/plugins/lv2/limiter_mono,http://lv2plug.in/plugins/eg-amp; do
      "$d/plugin_driver" --host "$d/sonora-plugin-host" --bundle "$d/lv2/sonora-test.lv2" \
        --scenario rtt --blocks 20000 --chain "$chain"
    done
  fi
' bash "${CTEST_ARGS[@]+"${CTEST_ARGS[@]}"}"

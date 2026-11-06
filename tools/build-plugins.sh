#!/usr/bin/env bash
# Build the whole CMake root with lilv present, so plugins/host is built, and
# run ctest: the plugin host tests against real LV2 and VST3 plugins from Debian, the
# in-tree misbehaving plugins, Sonora's own bundles (ADR-0011 N1: rendered
# through the host against the N0 references, lv2lint, the VST3 validator),
# and every other module's tests.
#
#   tools/build-plugins.sh                 build + full ctest
#   tools/build-plugins.sh -L plugins      build + only the plugin host tests
#   tools/build-plugins.sh --measure       build + a longer round-trip run
#   tools/build-plugins.sh --plugins-only  the plugin package's build (owner,
#                                          2026-11-05: the plugins are a product
#                                          of their own): only common/* and
#                                          plugins/native are configured
#                                          (SONORA_PLUGINS_ONLY), in
#                                          build-plugins-only/, and ctest runs
#                                          the native label: the processors'
#                                          suites, the format driver on every
#                                          bundle, lv2lint, the VST3 validator
#   tools/build-plugins.sh --plugins-only --stage
#                                          the same tree, bundles only, laid out
#                                          under build-plugins-only/stage for
#                                          tools/package-plugins.sh
#   tools/build-plugins.sh --stage         no tests: only sonora-plugin-host, the
#                                          offline tools and the native bundles,
#                                          laid out under build-plugins/stage for
#                                          tools/build-iso.sh
#
# Always in a container (sonora-plugins:trixie), so the plugin set is the one
# the tests were written against. The container gets CAP_SYS_NICE and RT
# limits, as in brain/research/adr-0007-measurements.md, so the driver and the
# host can run at SCHED_FIFO. Every timing it prints is a CONTAINER figure: a
# shared host kernel without PREEMPT_RT. It is not an appliance figure.
set -euo pipefail
# One heavy job at a time on this machine (tools/build-slot.sh).
[[ -n "${SONORA_HEAVY_HELD:-}" ]] || exec "$(dirname "${BASH_SOURCE[0]}")/build-slot.sh" "${BASH_SOURCE[0]}" "$@"

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
STAGE=0
ONLY=0
CTEST_ARGS=()
for arg in "$@"; do
  case "$arg" in
    --measure) MEASURE=1 ;;
    --stage) STAGE=1 ;;
    --plugins-only) ONLY=1 ;;
    *) CTEST_ARGS+=("$arg") ;;
  esac
done

# The plugins-only tree has its own directory, so it never reconfigures the
# full one; the container sees both as /src/<dir>.
if [[ "$ONLY" == 1 ]]; then
  BDIR=build-plugins-only
  CMAKE_ROOT_ARGS="-DSONORA_PLUGINS_ONLY=ON"
  if [[ ${#CTEST_ARGS[@]} -eq 0 ]]; then
    CTEST_ARGS=(-L native -j 4 --timeout 900)
    # A PR run skips the long DSP suites (tools/local-ci.sh).
    [[ -z "${SONORA_SKIP_SLOW:-}" ]] || CTEST_ARGS+=(-LE slow)
  fi
else
  BDIR=build-plugins
  CMAKE_ROOT_ARGS=""
fi

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

# CI builds the image itself, with its layers cached between runs (the
# validators' build stage takes minutes uncached), and sets
# SONORA_PLUGINS_IMAGE_BUILT=1 so it is not built a second time here.
if [[ "${SONORA_PLUGINS_IMAGE_BUILT:-0}" != 1 ]]; then
  docker build -q --label org.sonora=1 -t "$IMAGE" \
    -f "$(host_path "$ROOT/tools")/Dockerfile.plugins" "$(host_path "$ROOT/tools")" >/dev/null
fi

# The image build needs the host, the offline tools and the bundles, not the
# suite (SONORA_STAGE_ONLY in tools/build-engine.sh says why). Bundles go
# through the install rules, so an EXPERIMENTAL processor that has not met its
# CPU class (ADR-0011 3.5) is not shipped; only the plugin directories'
# cmake_install.cmake runs, because the root install needs every target built.
if [[ "$STAGE" == 1 ]]; then
  docker run --rm --label org.sonora=1 \
    -e BDIR="$BDIR" -e ONLY="$ONLY" -e ROOT_ARGS="$CMAKE_ROOT_ARGS" \
    -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -c '
    set -euo pipefail
    # shellcheck disable=SC2086
    cmake -S . -B "$BDIR" -DCMAKE_BUILD_TYPE=Release -DSONORA_REQUIRE_VST3=ON $ROOT_ARGS >/dev/null
    targets="sonora-plugin-host sonora_native_bundles"
    [[ "$ONLY" != 1 ]] || targets="sonora_native_bundles"
    # The offline passes (ADR-0011 4) when their targets exist.
    for t in sonora-analyze sonora-mixplan; do
      cmake --build "$BDIR" --target help | grep -q "^\.\.\. $t\$" && targets="$targets $t"
    done
    # shellcheck disable=SC2086
    cmake --build "$BDIR" -j"$(nproc)" --target $targets
    rm -rf $BDIR/stage
    mkdir -p $BDIR/stage/usr/local/bin
    for t in sonora-plugin-host sonora-analyze sonora-mixplan; do
      [[ -f $BDIR/$t ]] && install -m 0755 $BDIR/$t $BDIR/stage/usr/local/bin/$t
    done
    fmt="$(find "$BDIR" -path "*/plugins/native/format/cmake_install.cmake" | head -n 1)"
    [[ -n "$fmt" ]] || { echo "error: no install script for the native bundles" >&2; exit 1; }
    cmake -DCMAKE_INSTALL_PREFIX=/src/$BDIR/stage/usr/local -DCMAKE_INSTALL_CONFIG_NAME=Release -P "$fmt" >/dev/null
    test -d $BDIR/stage/usr/local/lib/lv2 || { echo "error: no LV2 bundle was installed" >&2; exit 1; }
    test -d $BDIR/stage/usr/local/lib/vst3 || { echo "error: no VST3 bundle was installed" >&2; exit 1; }
  '
  exit 0
fi

# The build directory is container-specific and ignored by git.
docker run --rm --label org.sonora=1 \
  --cap-add=SYS_NICE --ulimit rtprio=99 --ulimit memlock=-1 \
  ${SONORA_DSP_QUICK:+-e SONORA_DSP_QUICK=1}   -e MEASURE="$MEASURE" -e BDIR="$BDIR" -e ROOT_ARGS="$CMAKE_ROOT_ARGS" \
  -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -c '
  set -euo pipefail
  # shellcheck disable=SC2086
  cmake -S . -B "$BDIR" -DCMAKE_BUILD_TYPE=Release -DSONORA_REQUIRE_VST3=ON \
    -DSONORA_REQUIRE_PLUGIN_VALIDATORS=ON -DSONORA_REQUIRE_PLUGIN_EDITORS=ON $ROOT_ARGS >/dev/null
  cmake --build "$BDIR" -j"$(nproc)"
  ctest --test-dir "$BDIR" --output-on-failure "$@"
  if [[ "$MEASURE" == 1 ]]; then
    d="$BDIR"
    for chain in http://lv2plug.in/plugins/eg-amp \
                 http://gareus.org/oss/lv2/dpl#mono,http://lsp-plug.in/plugins/lv2/limiter_mono,http://lv2plug.in/plugins/eg-amp; do
      "$d/plugin_driver" --host "$d/sonora-plugin-host" --bundle "$d/lv2/sonora-test.lv2" \
        --scenario rtt --blocks 20000 --chain "$chain"
    done
  fi
' bash "${CTEST_ARGS[@]+"${CTEST_ARGS[@]}"}"

#!/usr/bin/env bash
# Run the plugin host's yabridge tests with Wine and yabridge present
# (ADR-0017 Y5): the plugins image plus tools/Dockerfile.yabridge. In CI's
# plugins image the Wine test skips (77); this is where it runs.
#
#   tools/test-yabridge.sh             build + ctest -R yabridge
#   tools/test-yabridge.sh -V          the same, ctest verbose (the timings)
#
# Builds into build-yabridge/ (ignored by git), apart from build-plugins/:
# this configure finds mingw-w64 and builds the Windows test plugin. Every
# timing it prints is a CONTAINER figure, not an appliance one.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-yabridge:trixie

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

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

TOOLS="$(host_path "$ROOT/tools")"
docker build -q --label org.sonora=1 -t sonora-plugins:trixie -f "$TOOLS/Dockerfile.plugins" "$TOOLS" >/dev/null
docker build -q --label org.sonora=1 -t "$IMAGE" -f "$TOOLS/Dockerfile.yabridge" "$TOOLS" >/dev/null

docker run --rm --label org.sonora=1 \
  --cap-add=SYS_NICE --ulimit rtprio=99 --ulimit memlock=-1 \
  -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -c '
  set -euo pipefail
  cmake -S . -B build-yabridge -DCMAKE_BUILD_TYPE=Release -DSONORA_REQUIRE_VST3=ON >/dev/null
  cmake --build build-yabridge -j"$(nproc)" --target sonora-plugin-host plugin_driver \
    sonora_test_vst3 sonora_test_vst3_bridged sonora_test_vst3_win >/dev/null
  ctest --test-dir build-yabridge --output-on-failure -R yabridge "$@"
' bash "$@"

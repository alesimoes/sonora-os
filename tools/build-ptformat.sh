#!/usr/bin/env bash
# Build the .ptx adapter against libptformat (ADR-0156) and run its test.
#
# libptformat is built from source at a pinned commit in tools/Dockerfile.ptformat;
# CMake only finds it on the system and never fetches it. This script builds
# that image on top of the engine toolchain image, then configures with
# -DSONORA_WITH_LIBPTFORMAT=ON inside the machine-wide build slot, builds
# test_ptx_libptformat and runs it. A configure that does not find the
# library is a failure here, not the silent fallback the default build takes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_IMAGE=sonora-build:trixie
IMAGE=sonora-ptformat:trixie

# Same Git Bash path handling as tools/build-engine.sh.
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

command -v docker >/dev/null || { echo "error: docker is not installed." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

TOOLS="$(host_path "$ROOT/tools")"
if [[ "${SONORA_BUILD_IMAGE_BUILT:-0}" != 1 ]]; then
  docker build -t "$BASE_IMAGE" --label org.sonora=1 -f "$TOOLS/Dockerfile.build" "$TOOLS"
fi
docker build -t "$IMAGE" --label org.sonora=1 --build-arg BASE="$BASE_IMAGE" \
  -f "$TOOLS/Dockerfile.ptformat" "$TOOLS"

# Its own build tree (build-container/ptformat, ignored like the rest of
# build-container/): the cache carries SONORA_WITH_LIBPTFORMAT=ON, which the
# plain engine build must not inherit. On Docker Desktop the tree lives on a
# named volume per checkout, as in tools/build-engine.sh.
JOBS="$("$ROOT/tools/build-slot.sh" --jobs)"
BUILD_VOL=()
if [[ "$(uname -s)" != Linux ]]; then
  ID="$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
  BUILD_VOL=(-v "sonora-ptformat-$ID:/build" -e SONORA_BUILD_DIR=/build/ptformat)
fi
# shellcheck disable=SC2016  # the script is expanded inside the container
"$ROOT/tools/build-slot.sh" docker run --rm \
  -e SONORA_JOBS="$JOBS" -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=10G \
  "${BUILD_VOL[@]}" -v sonora-ccache:/root/.ccache \
  -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -lc '
  set -euo pipefail
  B="${SONORA_BUILD_DIR:-build-container/ptformat}"
  export CMAKE_CXX_COMPILER_LAUNCHER=ccache CMAKE_C_COMPILER_LAUNCHER=ccache
  echo "libptformat $(pkg-config --modversion ptformat)"
  mkdir -p "$B"
  cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Release -DSONORA_WITH_LIBPTFORMAT=ON 2>&1 |
    tee "$B/configure.log"
  if grep -q "libptformat was not found" "$B/configure.log"; then
    echo "error: SONORA_WITH_LIBPTFORMAT=ON did not find libptformat" >&2
    exit 1
  fi
  cmake --build "$B" -j"$SONORA_JOBS" --target test_ptx_libptformat
  ctest --test-dir "$B" --output-on-failure -R "^interchange_ptx_libptformat\$"
'

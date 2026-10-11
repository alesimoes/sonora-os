#!/usr/bin/env bash
# Build and test the engine. Runs natively on Linux, in a container elsewhere.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-build:trixie

# Git Bash on Windows rewrites anything that looks like a Unix path before it
# reaches the executable, so a container path like /src arrives as
# C:/Program Files/Git/src. MSYS2_ARG_CONV_EXCL turns that off, which means we
# must hand Docker a Windows-shaped host path ourselves.
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

native_build() {
  cmake -S "$ROOT/engine" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$ROOT/build" -j"$(nproc)"
  ctest --test-dir "$ROOT/build" --output-on-failure
}

if [[ "$(uname -s)" == "Linux" ]] && command -v cmake >/dev/null && \
   pkg-config --exists jack 2>/dev/null; then
  native_build
  exit 0
fi

echo "Building in a container (no native toolchain with JACK headers found)."
command -v docker >/dev/null || {
  echo "error: docker is not installed, and this host cannot build natively." >&2
  exit 1
}
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

docker build -t "$IMAGE" -f "$(host_path "$ROOT/tools")/Dockerfile.build" "$(host_path "$ROOT/tools")"
# The build directory is container-specific; keep it out of the host tree.
docker run --rm -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -lc '
  set -euo pipefail
  cmake -S engine -B build-container -DCMAKE_BUILD_TYPE=Release
  cmake --build build-container -j"$(nproc)"
  ctest --test-dir build-container --output-on-failure
'

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

# A build tree configured before ADR-0007's M1 has engine/ as its source
# directory, and cmake refuses to reuse it for the root. It holds only build
# output, so it is started afresh rather than failing every old checkout once.
forget_pre_m1_tree() {
  if grep -qs '^CMAKE_HOME_DIRECTORY:INTERNAL=.*/engine$' "$1/CMakeCache.txt"; then
    rm -rf "$1"
  fi
}

native_build() {
  forget_pre_m1_tree "$ROOT/build"
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
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

docker build -t "$IMAGE" --label org.sonora=1 -f "$(host_path "$ROOT/tools")/Dockerfile.build" "$(host_path "$ROOT/tools")"
# The build directory is container-specific; keep it out of the host tree.
# Unlimited memlock, as the appliance grants it, so test_memory_lock can
# check that locking works and not only that a finite limit is refused.
docker run --rm --ulimit memlock=-1:-1 -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -lc '
  set -euo pipefail
  # forget_pre_m1_tree, inside the container where the tree was configured.
  if grep -qs "^CMAKE_HOME_DIRECTORY:INTERNAL=.*/engine\$" build-container/CMakeCache.txt; then
    rm -rf build-container
  fi
  cmake -S . -B build-container -DCMAKE_BUILD_TYPE=Release
  cmake --build build-container -j"$(nproc)"
  ctest --test-dir build-container --output-on-failure
'

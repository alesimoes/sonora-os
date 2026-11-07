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

# SONORA_STAGE_ONLY=1 builds the two binaries tools/build-iso.sh stages and
# runs no tests. The image build used to run the whole suite here, and a
# wall-clock CPU bar in a plugin's DSP suite, missed under load from other
# containers on the host, stopped the ISO after 33 minutes. The suite is CI's
# job and a plain tools/build-engine.sh run's; the image needs the binaries.
STAGE_ONLY="${SONORA_STAGE_ONLY:-0}"

native_build() {
  forget_pre_m1_tree "$ROOT/build"
  cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
  if [[ "$STAGE_ONLY" == 1 ]]; then
    cmake --build "$ROOT/build" -j"$(nproc)" --target sonora-engine sonora-tui sonora-ctl
    return
  fi
  cmake --build "$ROOT/build" -j"$(nproc)"
  ctest --test-dir "$ROOT/build" --output-on-failure -j"$(nproc)"
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

# CI builds the image beforehand with its layers in the Actions cache and
# sets SONORA_BUILD_IMAGE_BUILT=1 so it is not built a second time here.
if [[ "${SONORA_BUILD_IMAGE_BUILT:-0}" != 1 ]]; then
  docker build -t "$IMAGE" --label org.sonora=1 -f "$(host_path "$ROOT/tools")/Dockerfile.build" "$(host_path "$ROOT/tools")"
fi
# The build directory is container-specific; keep it out of the host tree.
# Unlimited memlock, as the appliance grants it, so test_memory_lock can
# check that locking works and not only that a finite limit is refused.
# One of a few global build slots (tools/build-slot.sh), -j the share of the
# CPUs one slot owns, and a ccache shared by every worktree: /src is the same
# path in all of them, so their objects hit each other's cache.
#
# On Docker Desktop /src is a bind mount of an NTFS folder, and every file
# operation of a configure or a compile crosses that boundary. There the build
# tree lives on a named volume per checkout (reused, so builds stay
# incremental) and only the finished executables are copied back to
# build-container/, where tools/build-iso.sh and the interop tests look. On a
# Linux host the bind mount is native and the tree stays in the checkout.
JOBS="$("$ROOT/tools/build-slot.sh" --jobs)"
BUILD_VOL=()
if [[ "$(uname -s)" != Linux ]]; then
  ID="$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
  BUILD_VOL=(-v "sonora-build-$ID:/build" -e SONORA_BUILD_DIR=/build/container)
fi
# shellcheck disable=SC2016  # the script is expanded inside the container
"$ROOT/tools/build-slot.sh" docker run --rm --ulimit memlock=-1:-1 \
  -e SONORA_STAGE_ONLY="$STAGE_ONLY" -e SONORA_JOBS="$JOBS" \
  ${SONORA_DSP_QUICK:+-e SONORA_DSP_QUICK="$SONORA_DSP_QUICK"} \
  -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=10G "${BUILD_VOL[@]}" \
  -v sonora-ccache:/root/.ccache -v "$(host_path "$ROOT"):/src" -w /src "$IMAGE" bash -lc '
  set -euo pipefail
  B="${SONORA_BUILD_DIR:-build-container}"
  export CMAKE_CXX_COMPILER_LAUNCHER=ccache CMAKE_C_COMPILER_LAUNCHER=ccache
  ccache -z >/dev/null
  finish() {
    ccache -s | grep -iE "hit|cache size" || true
    if [[ "$B" != build-container ]]; then
      mkdir -p build-container
      find "$B" -maxdepth 1 -type f -perm -u+x -exec cp -f {} build-container/ \;
    fi
  }
  trap finish EXIT
  # forget_pre_m1_tree, inside the container where the tree was configured.
  if grep -qs "^CMAKE_HOME_DIRECTORY:INTERNAL=.*/engine\$" "$B/CMakeCache.txt"; then
    rm -rf "$B"
  fi
  cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Release
  if [[ "$SONORA_STAGE_ONLY" == 1 ]]; then
    cmake --build "$B" -j"$SONORA_JOBS" --target sonora-engine sonora-tui sonora-ctl
    exit 0
  fi
  cmake --build "$B" -j"$SONORA_JOBS"
  ctest --test-dir "$B" --output-on-failure -j"$(nproc)"
'

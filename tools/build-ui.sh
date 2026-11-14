#!/usr/bin/env bash
# Build and test the Qt interface (ui/qt) in the sonora-ui container.
#
# tools/build-engine.sh builds the same CMake tree in an image with no Qt, so
# there ui/qt is skipped with one status line. This builds the whole tree in
# tools/Dockerfile.ui, where Qt 6 is installed, and runs every ctest test
# with the UI ones included -- headless, on Qt's offscreen platform.
#
#   tools/build-ui.sh                       build, then ctest
#   tools/build-ui.sh --stage               only the sonora-ui binary, no
#                                           tests (for tools/build-iso.sh)
#   tools/build-ui.sh --screenshot PATH     ... then one picture per scene in
#                                           ui/qt/src/scenes/ (the Edit window
#                                           is PATH; sonora-ui --list-scenes
#                                           names the rest) under the cage
#                                           kiosk compositor, captured by the
#                                           compositor itself (grim); and,
#                                           on a sonora-engine announcing a
#                                           newer protocol, the version
#                                           refusal at startup (-refusal) and
#                                           on reconnect (-refusal-reconnect)
#                                           Builds only sonora-ui and sonora-engine
#                                           and runs no tests; the build tree and
#                                           ccache are kept, so a rerun is
#                                           incremental.
#   tools/build-ui.sh --measure [SECONDS]   ... then frame time and CPU cost
#                                           of the shell, meters on and off,
#                                           under Xvfb and under cage
#
# Everything here renders with llvmpipe, a CPU rasteriser. The screenshot is
# a faithful picture; the measurements are the cost of drawing on a CPU,
# which ADR-0006 says is not a supported configuration. They are not a
# frame-rate claim about the target, and the report says so.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-ui:trixie

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

MODE="test"
SHOT=""
SECONDS_EACH=20
while [[ $# -gt 0 ]]; do
  case "$1" in
    --screenshot)
      [[ $# -ge 2 ]] || { echo "error: --screenshot needs a path" >&2; exit 2; }
      MODE=screenshot
      SHOT="$2"
      shift 2
      ;;
    --stage)
      MODE=stage
      shift
      ;;
    --measure)
      MODE=measure
      if [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]]; then
        SECONDS_EACH="$2"
        shift
      fi
      shift
      ;;
    -h|--help)
      # The header comment, however long it grows: line 2 to the first
      # line that is not a comment.
      awk 'NR > 1 && !/^#/ { exit } NR > 1' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "error: unknown argument $1" >&2
      exit 2
      ;;
  esac
done

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

# Dockerfile.ui expects the repository root as its context, for the one file
# it copies out of audio-stack/. The root also holds build trees and, in a
# checkout that has built the image, an ISO and live-build's cache; sending
# all that to the daemon on every run costs minutes. So the context is a
# scratch directory holding exactly what the Dockerfile reads.
#
# CI builds the image itself, with its layers cached between runs, and sets
# SONORA_UI_IMAGE_BUILT=1 so it is not built a second time here, uncached.
if [[ "${SONORA_UI_IMAGE_BUILT:-0}" != 1 ]]; then
  CONTEXT="$(mktemp -d)"
  trap 'rm -rf "$CONTEXT"' EXIT
  mkdir -p "$CONTEXT/audio-stack"
  cp "$ROOT/tools/Dockerfile.ui" "$CONTEXT/Dockerfile"
  cp "$ROOT/audio-stack/pipewire-sonora.conf" "$CONTEXT/audio-stack/"
  docker build -t "$IMAGE" --label org.sonora=1 "$(host_path "$CONTEXT")"
fi

OUT_MOUNT=()
SHOT_NAME=""
if [[ "$MODE" == screenshot ]]; then
  shot_dir="$(dirname "$SHOT")"
  mkdir -p "$shot_dir"
  SHOT_NAME="$(basename "$SHOT")"
  OUT_MOUNT=(-v "$(host_path "$shot_dir"):/out")
fi

# On a Linux host the bind mount keeps host ownership, so the build tree must
# be written as the caller: the image's own user (uid 1000) cannot write a
# checkout owned by anyone else, such as the CI runner's uid 1001. Docker
# Desktop maps ownership itself.
AS_CALLER=()
if [[ "$(uname -s)" == Linux ]]; then
  AS_CALLER=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
fi

# One of a few global build slots (tools/build-slot.sh), -j its share of the
# CPUs, and the ccache every worktree shares (/src is the same path in all),
# so a new checkout or a bisect half compiles from cache, not for 30 minutes.
# On a Linux host the container runs as the caller, who may not be able to
# write the volume: ccache then finds no usable cache and compiles normally.
# Off Linux the container runs as the image user (uid 1000), but the volume
# may hold files tools/build-engine.sh wrote as root, so they are handed over.
JOBS="$("$ROOT/tools/build-slot.sh" --jobs)"
# Off a Linux host the tree is on a volume per checkout, not in the NTFS bind
# mount (see tools/build-engine.sh).
BUILD_VOL=()
if [[ "$(uname -s)" != Linux ]]; then
  ID="$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
  BUILD_VOL=(-v "sonora-build-$ID:/build" -e SONORA_BUILD_DIR=/build/ui)
fi
if [[ "$(uname -s)" != Linux ]]; then
  docker run --rm --user 0 -v sonora-ccache:/ccache --entrypoint find "$IMAGE" \
    /ccache ! -user 1000 -exec chown 1000:1000 {} + >/dev/null 2>&1 || true
  # The per-checkout volume is shared with tools/build-engine.sh, which runs as
  # root: when it ran first (tools/build-iso.sh builds the engine, then the UI)
  # /build is root's and the image user cannot create /build/ui in it.
  docker run --rm --user 0 "${BUILD_VOL[@]}" --entrypoint sh "$IMAGE" -c \
    'mkdir -p /build/ui && chown 1000:1000 /build && find /build/ui ! -user 1000 -exec chown 1000:1000 {} +' \
    >/dev/null 2>&1 || true
fi
# On a Linux host the container runs as the caller, so the cache must be a
# host directory the caller owns: a named volume starts out as root's and
# ccache then fails with "Permission denied" inside every compiler check
# (the nightly's UI and ISO builds, 2026-11-07; same cause as micro-build.sh).
if [[ "$(uname -s)" == Linux ]]; then
  CCACHE_HOST_DIR="${SONORA_CCACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/sonora-ccache}"
  mkdir -p "$CCACHE_HOST_DIR"
  CCACHE_MOUNT=(-v "$CCACHE_HOST_DIR:/ccache")
else
  CCACHE_MOUNT=(-v sonora-ccache:/ccache)
fi
"$ROOT/tools/build-slot.sh" docker run --rm "${AS_CALLER[@]}" \
  -e LANG=C.UTF-8 -e SONORA_JOBS="$JOBS" -e SONORA_UI_SCOPE="${SONORA_UI_SCOPE:-all}" \
  ${SONORA_DSP_QUICK:+-e SONORA_DSP_QUICK="$SONORA_DSP_QUICK"} -e SONORA_SKIP_SLOW="${SONORA_SKIP_SLOW:-0}" -e SONORA_CTEST_LABELS="${SONORA_CTEST_LABELS:-}" \
  -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=10G "${BUILD_VOL[@]}" \
  "${CCACHE_MOUNT[@]}" \
  -v "$(host_path "$ROOT"):/src" "${OUT_MOUNT[@]}" -w /src "$IMAGE" \
  tools/build-ui-entrypoint.sh "$MODE" "$SHOT_NAME" "$SECONDS_EACH"

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
# CPUs, and the ccache every worktree shares (/src is the same path in all).
# On a Linux host the container runs as the caller, who cannot write the
# volume's root-owned /root/.ccache: ccache then finds no usable cache and
# compiles normally.
JOBS="$("$ROOT/tools/build-slot.sh" --jobs)"
# Off a Linux host the tree is on a volume per checkout, not in the NTFS bind
# mount (see tools/build-engine.sh).
BUILD_VOL=()
if [[ "$(uname -s)" != Linux ]]; then
  ID="$(printf %s "${ROOT##*/}" | tr -c 'A-Za-z0-9_.-' '_')"
  BUILD_VOL=(-v "sonora-build-$ID:/build" -e SONORA_BUILD_DIR=/build/ui)
fi
"$ROOT/tools/build-slot.sh" docker run --rm "${AS_CALLER[@]}" \
  -e LANG=C.UTF-8 -e SONORA_JOBS="$JOBS" -e SONORA_UI_SCOPE="${SONORA_UI_SCOPE:-all}" -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=10G "${BUILD_VOL[@]}" \
  -v sonora-ccache:/root/.ccache \
  -v "$(host_path "$ROOT"):/src" "${OUT_MOUNT[@]}" -w /src "$IMAGE" \
  tools/build-ui-entrypoint.sh "$MODE" "$SHOT_NAME" "$SECONDS_EACH"

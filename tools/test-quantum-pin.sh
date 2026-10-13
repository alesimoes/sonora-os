#!/usr/bin/env bash
# Prove that nothing a client can do moves the graph off the pinned quantum
# and rate (ADR-0003). Under ADR-0007 M2 a buffer-size or rate change in the
# middle of a take is fatal to the take, so "the default is 128" is not enough:
# a second JACK client asking for 64 frames must not get them.
#
# Runs tools/test-quantum-entrypoint.sh inside the same runtime image as
# tools/test-engine-docker.sh, which carries audio-stack/pipewire-sonora.conf
# exactly as the ISO installs it. No engine build is needed: the observer is a
# small JACK client (tools/jack-quantum-probe.py) going through the same
# pipewire-jack the engine links against.
#
# What this cannot prove: behaviour with a real ALSA driver node, whose
# hardware may have its own opinion about period sizes. The container has only
# PipeWire's dummy driver.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-runtime:trixie

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

echo "=== building the runtime image ==="
# Run by image ID, not by tag. The tag is shared by every worktree on this
# Docker host, and another checkout rebuilding it between our build and our
# run would test its configuration under our name. That happened.
IMAGE_ID="$(docker build -q -t "$IMAGE" \
  -f "$(host_path "$ROOT/tools")/Dockerfile.runtime" \
  "$(host_path "$ROOT")")"

echo "=== trying to move the graph off 128 frames at 48000 Hz ==="
docker run --rm --entrypoint /usr/local/bin/test-quantum-entrypoint.sh "$IMAGE_ID"

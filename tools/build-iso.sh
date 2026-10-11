#!/usr/bin/env bash
# Build the bootable image with live-build. Needs Linux and root, so it runs
# in a privileged container on any other host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-livebuild:trixie

run_live_build() {
  cd "$ROOT/iso"
  lb clean --purge
  lb config        # reads config/ in this directory
  lb build
  mv -f live-image-amd64.hybrid.iso sonora.iso
  echo "built: $ROOT/iso/sonora.iso"
}

if [[ "$(uname -s)" == "Linux" && "$(id -u)" == "0" ]] && command -v lb >/dev/null; then
  run_live_build
  exit 0
fi

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

docker build -t "$IMAGE" -f "$ROOT/tools/Dockerfile.livebuild" "$ROOT/tools"
# live-build needs to mount, create loop devices and chroot: hence privileged.
docker run --rm --privileged \
  -v "$ROOT:/src" -w /src/iso "$IMAGE" bash -lc '
  set -euo pipefail
  lb clean --purge
  lb config
  lb build
  mv -f live-image-amd64.hybrid.iso sonora.iso
  echo "built: iso/sonora.iso"
'

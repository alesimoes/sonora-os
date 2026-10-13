#!/usr/bin/env bash
# Build the bootable image with live-build. Needs Linux and root, so it runs
# in a privileged container on any other host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-livebuild:trixie
VOLUME=sonora-livebuild-work

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

if [[ "$(uname -s)" == "Linux" && "$(id -u)" == "0" ]] && command -v lb >/dev/null; then
  cd "$ROOT/iso"
  lb clean
  lb config
  lb build
  mv -f live-image-amd64.hybrid.iso sonora.iso
  echo "built: $ROOT/iso/sonora.iso"
  exit 0
fi

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

docker build -t "$IMAGE" --label org.sonora=1 \
  -f "$(host_path "$ROOT/tools")/Dockerfile.livebuild" \
  "$(host_path "$ROOT/tools")"

# The systemd unit runs /usr/local/bin/sonora-engine. Nothing in live-build
# produces that, so the engine is built first and staged into
# config/includes.chroot, where live-build copies it into the image verbatim.
# Without this the image boots into a restart loop on a missing binary.
echo "Building the engine for the image..."
"$ROOT/tools/build-engine.sh" >/dev/null

STAGE="$ROOT/iso/config/includes.chroot/usr/local/bin"
mkdir -p "$STAGE"
ENGINE=""
for candidate in "$ROOT/build/sonora-engine" "$ROOT/build-container/sonora-engine"; do
  # -f, not -x: a Windows filesystem does not carry an exec bit, so the
  # freshly built binary shows up as mode 644 here.
  [[ -f "$candidate" ]] && ENGINE="$candidate"
done
[[ -n "$ENGINE" ]] || {
  echo "error: the engine was built but no binary was found to stage." >&2
  exit 1
}
install -m 0755 "$ENGINE" "$STAGE/sonora-engine"
# The terminal interface, a separate process since ADR-0007 M7: the engine
# starts it from beside itself on tty1.
install -m 0755 "$(dirname "$ENGINE")/sonora-tui" "$STAGE/sonora-tui"
echo "staged the engine and sonora-tui into includes.chroot/usr/local/bin/"


# live-build must not work on the bind-mounted host directory. debootstrap
# unpacks with tar, which needs real ownership, hardlinks and xattrs; on a
# Windows bind mount that fails with "Tried to extract package, but tar
# failed". So the build happens in a Docker volume, which is ext4 inside the
# Linux VM, and only the finished ISO is copied back out.
# Kept between runs on purpose: it holds live-build's package cache.
# tools/clean.sh removes it only with --deep.
docker volume create --label org.sonora=1 "$VOLUME" >/dev/null

docker run --rm --privileged \
  -v "$(host_path "$ROOT"):/src:ro" \
  -v "$VOLUME:/work" \
  -w /work "$IMAGE" bash -lc '
  set -euo pipefail

  mkdir -p /work/build
  cd /work/build

  # Refresh the configuration from the repository on every run, but leave
  # cache/ alone: re-downloading the Debian base each time turns a two-minute
  # rebuild back into twenty.
  rm -rf auto config
  cp -r /src/iso/auto /src/iso/config .

  lb clean
  lb config
  lb build

  cp -f live-image-amd64.hybrid.iso /work/sonora.iso
  echo "iso ready in the volume"
'

# A second, unprivileged run copies the result to the host. Keeping this out of
# the build step means a failed build cannot leave a half-written ISO behind.
docker run --rm \
  -v "$(host_path "$ROOT/iso"):/out" \
  -v "$VOLUME:/work" \
  "$IMAGE" bash -lc 'cp -f /work/sonora.iso /out/sonora.iso'

ls -lh "$ROOT/iso/sonora.iso"
echo "built: $ROOT/iso/sonora.iso"

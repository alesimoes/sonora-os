#!/usr/bin/env bash
# Build the bootable image with live-build. Needs Linux and root, so it runs
# in a privileged container on any other host.
set -euo pipefail
# One heavy job at a time on this machine (tools/build-slot.sh).
[[ -n "${SONORA_HEAVY_HELD:-}" ]] || exec "$(dirname "${BASH_SOURCE[0]}")/build-slot.sh" "${BASH_SOURCE[0]}" "$@"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-livebuild:trixie
# SONORA_LB_VOLUME: a separate volume, e.g. to time a cold build beside the
# cached one.
VOLUME="${SONORA_LB_VOLUME:-sonora-livebuild-work}"

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

# BUILD_ID for /etc/os-release (the plugin catalog cache keys on it, ADR-0012).
# Commit date plus short hash, not the wall clock: the same commit must give
# the same id, or every rebuild would discard every operator's cache. A dirty
# tree gets a suffix so an uncommitted experiment never shares a commit's id.
#
# git.exe is a native Windows program and, with MSYS2_ARG_CONV_EXCL set above,
# gets /c/... unconverted and cannot find it: every image built on Windows
# was "unstamped" until the path went through host_path.
stamp_build_id() {
  local id date sha repo
  repo="$(host_path "$ROOT")"
  if date="$(git -C "$repo" log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null)" &&
     sha="$(git -C "$repo" rev-parse --short=9 HEAD 2>/dev/null)"; then
    id="$date.$sha"
    [[ -z "$(git -C "$repo" status --porcelain -- iso audio-stack engine 2>/dev/null)" ]] || id="$id-dirty"
  else
    id="unstamped"
  fi
  mkdir -p "$ROOT/iso/config/includes.chroot/usr/share/sonora"
  printf '%s\n' "$id" >"$ROOT/iso/config/includes.chroot/usr/share/sonora/build-id"
  echo "BUILD_ID=$id"
}
stamp_build_id

# iso/sonora.iso.sha256, in sha256sum's format. tools/write-usb.ps1 refuses to
# erase a stick for an image that no longer matches it: a copy cut short by a
# full disk or an interrupted transfer can still boot partway, which is worse
# than not booting.
write_sidecar() {
  (cd "$ROOT/iso" && rm -f sonora.iso.sha256 && sha256sum sonora.iso > sonora.iso.sha256)
  cat "$ROOT/iso/sonora.iso.sha256"
}

if [[ "$(uname -s)" == "Linux" && "$(id -u)" == "0" ]] && command -v lb >/dev/null; then
  cd "$ROOT/iso"
  lb clean
  lb config
  lb build
  mv -f live-image-amd64.hybrid.iso sonora.iso
  write_sidecar
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

# The engine and UI builds print thousands of lines, so they go to a log
# under iso/ (ignored by git) rather than the terminal. Not to /dev/null: a
# failure there was silent apart from the exit status, which left nothing to
# diagnose from (#217 review). On failure the tail is shown and the build stops.
run_logged() {
  local name="$1" log="$ROOT/iso/$1.log"
  shift
  if ! "$@" >"$log" 2>&1; then
    echo "error: $name failed; the last 40 lines of $log:" >&2
    tail -n 40 "$log" >&2
    exit 1
  fi
  echo "$name: ok (log: $log)"
}

# The systemd unit runs /usr/local/bin/sonora-engine. Nothing in live-build
# produces that, so the engine is built first and staged into
# config/includes.chroot, where live-build copies it into the image verbatim.
# Without this the image boots into a restart loop on a missing binary.
echo "Building the engine for the image..."
# Binaries only (SONORA_STAGE_ONLY): the tests are CI's, and a timing bar
# missed under host load must not cost the image.
run_logged build-engine env SONORA_STAGE_ONLY=1 "$ROOT/tools/build-engine.sh"

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

# The graphical interface (ADR-0010) needs Qt, which the engine image lacks, so
# it is built by tools/build-ui.sh and staged only on request: SONORA_WITH_UI=1.
# Without it any stale copy is removed, so an image never ships a binary from
# an older commit; sonora-ui.service stays inert without sonora.gui anyway.
rm -f "$STAGE/sonora-ui"
if [[ "${SONORA_WITH_UI:-0}" == 1 ]]; then
  echo "Building the interface for the image..."
  run_logged build-ui "$ROOT/tools/build-ui.sh" --stage
  UI="$(find "$ROOT/build-ui-container" -name sonora-ui -type f 2>/dev/null | head -n 1)"
  [[ -n "$UI" ]] || {
    echo "error: SONORA_WITH_UI=1 but no sonora-ui binary was found to stage." >&2
    exit 1
  }
  install -m 0755 "$UI" "$STAGE/sonora-ui"
  echo "staged sonora-ui"
fi


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

  # cache/bootstrap is a whole debootstrapped tree, reused as is: after a
  # snapshot bump it would keep the old base (and kernel) under the new
  # timestamp. So the cache belongs to one snapshot and goes with it.
  snap="$(sed -n "s/^SONORA_SNAPSHOT=//p" auto/config)"
  if [ "$(cat cache/sonora-snapshot 2>/dev/null || true)" != "$snap" ]; then
    echo "snapshot $snap: dropping the package cache of another snapshot"
    rm -rf cache
  fi
  mkdir -p cache
  echo "$snap" > cache/sonora-snapshot

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

write_sidecar
ls -lh "$ROOT/iso/sonora.iso"
echo "built: $ROOT/iso/sonora.iso"

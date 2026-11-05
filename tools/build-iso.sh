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

# shellcheck source=tools/iso-preflight.sh
. "$ROOT/tools/iso-preflight.sh"

# SONORA_DEV_SNAPSHOT=<YYYYMMDDTHHMMSSZ>: a development build from another
# snapshot when the committed one is unreachable (snapshot.debian.org has been
# down for hours). The pins must still resolve there. It is not a release: the
# image is named sonora-dev.iso, its BUILD_ID says -dev, and SONORA_RELEASE=1
# refuses it. Unset, nothing here changes.
DEV_SNAPSHOT="$(pf_dev_env)" || exit 1
OUT_NAME=sonora.iso
if [[ -n "$DEV_SNAPSHOT" ]]; then
  OUT_NAME=sonora-dev.iso
  echo "WARNING: DEVELOPMENT BUILD from snapshot $DEV_SNAPSHOT, not the committed one. Output: iso/$OUT_NAME. Not a release." >&2
fi
CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

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
    [[ -z "$DEV_SNAPSHOT" ]] || id="$id-dev"
  else
    id="unstamped"
  fi
  mkdir -p "$ROOT/iso/config/includes.chroot/usr/share/sonora"
  printf '%s\n' "$id" >"$ROOT/iso/config/includes.chroot/usr/share/sonora/build-id"
  echo "BUILD_ID=$id"
}

# --list-staged: what iso/staged.list says the image carries beyond the tracked
# files, and what is staged in includes.chroot right now with its size. Builds
# nothing, needs no Docker.
list_staged() {
  local path kind by size target total=0 bytes
  printf '%-40s %-9s %-10s %s\n' PATH KIND STAGED "BUILT BY"
  while read -r path kind by; do
    case "$path" in ''|'#'*) continue ;; esac
    target="$ROOT/iso/config/includes.chroot$path"
    if [[ -e "$target" ]]; then
      bytes="$(du -sk "$target" | cut -f1)"
      total=$((total + bytes))
      size="${bytes}K"
    else
      size="no"
    fi
    printf '%-40s %-9s %-10s %s\n' "$path" "$kind" "$size" "$by"
  done <"$ROOT/iso/staged.list"
  echo "staged now: ${total}K (du -sk, before squashfs compression)"
}
case "${1:-}" in
  --list-staged) list_staged; exit 0 ;;
  ''|--check) ;;
  *) echo "usage: tools/build-iso.sh [--check|--list-staged]" >&2; exit 2 ;;
esac

[[ "$CHECK_ONLY" == 1 ]] || stamp_build_id

# iso/sonora.iso.sha256, in sha256sum's format. tools/write-usb.ps1 refuses to
# erase a stick for an image that no longer matches it: a copy cut short by a
# full disk or an interrupted transfer can still boot partway, which is worse
# than not booting.
write_sidecar() {
  (cd "$ROOT/iso" && rm -f "$OUT_NAME.sha256" && sha256sum "$OUT_NAME" > "$OUT_NAME.sha256")
  cat "$ROOT/iso/$OUT_NAME.sha256"
}

if [[ "$(uname -s)" == "Linux" && "$(id -u)" == "0" ]] && command -v lb >/dev/null; then
  if [[ "$CHECK_ONLY" == 1 ]]; then
    pf_inside
    exit $?
  fi
  pf_inside
  cd "$ROOT/iso"
  if [[ -n "$DEV_SNAPSHOT" ]]; then
    echo "error: a development snapshot needs the container path (it rewrites a copy of the configuration)." >&2
    exit 1
  fi
  lb clean
  lb config
  lb build
  mv -f live-image-amd64.hybrid.iso "$OUT_NAME"
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

# --check: resolve every package and pin in a trixie container and stop. About
# a minute, no chroot, no engine build (tools/iso-preflight.sh).
if [[ "$CHECK_ONLY" == 1 ]]; then
  docker run --rm -e SONORA_DEV_SNAPSHOT="$DEV_SNAPSHOT" -e SONORA_RETRY_DELAY \
    -v "$(host_path "$ROOT"):/src:ro" "$IMAGE" bash /src/tools/iso-preflight.sh inside
  exit $?
fi

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
# it is built by tools/build-ui.sh. Staged by default now that stage 2 starts
# it at boot; SONORA_WITH_UI=0 skips it (a faster build of an image without
# the display stack, whose unit is inert without sonora.gui anyway). Any stale
# copy is removed first, so an image never ships a binary from an older commit.
rm -f "$STAGE/sonora-ui"
if [[ "${SONORA_WITH_UI:-1}" == 1 ]]; then
  echo "Building the interface for the image..."
  run_logged build-ui "$ROOT/tools/build-ui.sh" --stage
  UI="$(find "$ROOT/build-ui-container" -name sonora-ui -type f 2>/dev/null | head -n 1)"
  [[ -n "$UI" ]] || {
    echo "error: the interface was built but no sonora-ui binary was found to stage." >&2
    exit 1
  }
  install -m 0755 "$UI" "$STAGE/sonora-ui"
  echo "staged sonora-ui"
fi

# The plugin host and the native LV2 and VST3 bundles, built in the
# sonora-plugins image (the only one with lilv and the VST3 SDK). Without the
# host the engine starts without --plugin-host (sonora-engine-run) and every
# insert is silently unavailable. Bundles go to /usr/local/lib/{lv2,vst3}, on
# the default search path of lilv and of the host's VST3 loader.
# SONORA_WITH_PLUGINS=0 skips all of it.
INCLUDES="$ROOT/iso/config/includes.chroot"
rm -f "$STAGE/sonora-plugin-host" "$STAGE/sonora-analyze" "$STAGE/sonora-mixplan"
rm -rf "$INCLUDES/usr/local/lib/lv2" "$INCLUDES/usr/local/lib/vst3"
if [[ "${SONORA_WITH_PLUGINS:-1}" == 1 ]]; then
  echo "Building the plugin host and the native bundles for the image..."
  run_logged build-plugins "$ROOT/tools/build-plugins.sh" --stage
  PSTAGE="$ROOT/build-plugins/stage/usr/local"
  [[ -f "$PSTAGE/bin/sonora-plugin-host" ]] || {
    echo "error: the plugins were built but no sonora-plugin-host was found to stage." >&2
    exit 1
  }
  for tool in "$PSTAGE"/bin/*; do
    install -m 0755 "$tool" "$STAGE/$(basename "$tool")"
  done
  mkdir -p "$INCLUDES/usr/local/lib"
  cp -r "$PSTAGE/lib/lv2" "$PSTAGE/lib/vst3" "$INCLUDES/usr/local/lib/"
  echo "staged sonora-plugin-host and the bundles into includes.chroot/usr/local/"
fi

# The Keys menu's profiles: sonora-ui reads /usr/share/sonora/keymaps.
mkdir -p "$INCLUDES/usr/share/sonora/keymaps"
install -m 0644 "$ROOT"/keymaps/*.json "$INCLUDES/usr/share/sonora/keymaps/"

list_staged


# live-build must not work on the bind-mounted host directory. debootstrap
# unpacks with tar, which needs real ownership, hardlinks and xattrs; on a
# Windows bind mount that fails with "Tried to extract package, but tar
# failed". So the build happens in a Docker volume, which is ext4 inside the
# Linux VM, and only the finished ISO is copied back out.
# Kept between runs on purpose: it holds live-build's package cache.
# tools/clean.sh removes it only with --deep.
docker volume create --label org.sonora=1 "$VOLUME" >/dev/null

docker run --rm --privileged -e SONORA_DEV_SNAPSHOT="$DEV_SNAPSHOT" -e SONORA_RETRY_DELAY \
  -e OUT_NAME="$OUT_NAME" \
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

  # Fail in a minute, not after the bootstrap: unreachable snapshot, a pin or
  # the RT kernel that does not resolve (tools/iso-preflight.sh).
  PF_ROOT=/src bash /src/tools/iso-preflight.sh inside

  # Development fallback: the one timestamp in auto/config and in the
  # backports list, which the hook requires to be equal.
  if [ -n "${SONORA_DEV_SNAPSHOT:-}" ]; then
    old="$(sed -n "s/^SONORA_SNAPSHOT=//p" auto/config)"
    sed -i "s/$old/$SONORA_DEV_SNAPSHOT/" auto/config config/archives/sonora-backports.list.chroot
  fi

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

  cp -f live-image-amd64.hybrid.iso "/work/$OUT_NAME"
  echo "iso ready in the volume"
'

# A second, unprivileged run copies the result to the host. Keeping this out of
# the build step means a failed build cannot leave a half-written ISO behind.
docker run --rm \
  -v "$(host_path "$ROOT/iso"):/out" \
  -v "$VOLUME:/work" \
  -e OUT_NAME="$OUT_NAME" "$IMAGE" bash -lc 'cp -f "/work/$OUT_NAME" "/out/$OUT_NAME"'

write_sidecar
ls -lh "$ROOT/iso/$OUT_NAME"
echo "built: $ROOT/iso/$OUT_NAME"

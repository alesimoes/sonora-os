#!/usr/bin/env bash
# Build iso/sonora-stick.img: the whole stick as one file -- the ISO followed
# by the persistence partition (ADR-0005) -- for a writer that can only copy
# an image byte for byte. That is tools/write-usb.ps1 on Windows, which has no
# way to make an ext4 filesystem.
#
#   tools/make-stick-image.sh                    # 4096 MiB of persistence
#   tools/make-stick-image.sh --persistence-mib 24000
#
# The image is made by tools/write-usb.sh itself, run against a loop device
# over a sparse file in a Docker volume, exactly as tools/test-write-usb.sh
# runs it. There is no second implementation of the partition step to drift.
# Nothing here opens a real disk: SONORA_WRITE_USB_TEST_IMAGE makes
# write-usb.sh accept a loop device over that one file and nothing else.
#
# The persistence partition is as large as asked, not as large as the stick:
# the image is written whole, so a 32 GB partition is a 32 GB write. The rest
# of a bigger stick stays unallocated.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
IMAGE=sonora-usbtest:trixie
VOLUME="sonora-stick-work-$(date +%s)-$$"
PERSIST_MIB=4096

die() { echo "error: $*" >&2; exit 1; }

while (( $# )); do
  case "$1" in
    --persistence-mib) [[ $# -ge 2 ]] || die "--persistence-mib needs a value."; PERSIST_MIB="$2"; shift 2 ;;
    *) die "unknown argument $1 (usage: $0 [--persistence-mib N])" ;;
  esac
done
[[ "$PERSIST_MIB" =~ ^[0-9]+$ ]] || die "--persistence-mib takes a whole number of MiB."
# write-usb.sh refuses below 256 MiB; saying so here costs no container run.
(( PERSIST_MIB >= 256 )) || die "--persistence-mib must be at least 256."

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

[[ -f "$ISO" ]] || die "$ISO not found. Run tools/build-iso.sh first."
if [[ -f "$ISO.sha256" ]]; then
  (cd "$ROOT/iso" && sha256sum -c --quiet sonora.iso.sha256) \
    || die "$ISO does not match sonora.iso.sha256. Rebuild it: tools/build-iso.sh"
fi
command -v docker >/dev/null || die "docker is required."
docker info >/dev/null 2>&1 || die "the Docker daemon is not reachable. Start Docker Desktop."

docker build -q -t "$IMAGE" --label org.sonora=1 \
  -f "$(host_path "$ROOT/tools")/Dockerfile.usbtest" \
  "$(host_path "$ROOT/tools")" >/dev/null
trap 'docker volume rm -f "$VOLUME" >/dev/null 2>&1 || true' EXIT
docker volume create --label org.sonora=1 "$VOLUME" >/dev/null

rm -f "$ROOT/iso/sonora-stick.img" "$ROOT/iso/sonora-stick.img.sha256" "$ROOT/iso/sonora-stick.img.part"

# --privileged lets the container see every block device of the Docker host
# (on Docker Desktop, the Linux VM's virtual disks; on a CI runner, the
# runner's own disk), with or without the devtmpfs mount below: Docker copies
# the host's device nodes into a privileged container itself. So the guard is
# write-usb.sh's SONORA_WRITE_USB_TEST_IMAGE check, which accepts only a loop
# device backed by exactly that file. Narrowing the container to loop devices
# by cgroup rule does not work: loop partitions take dynamic majors (259,
# blkext), which NVMe namespaces share. Recorded as a follow-up in the PR.
#
# The sparse file lives in the volume (ext4 in the Linux VM): a loop device
# over a Windows bind mount is not something to depend on. Only the finished
# image is copied out, under a .part name until it is complete.
docker run --rm --privileged \
  -v "$(host_path "$ROOT"):/src:ro" \
  -v "$(host_path "$ROOT/iso"):/out" \
  -v "$VOLUME:/work" \
  -e PERSIST_MIB="$PERSIST_MIB" \
  "$IMAGE" bash -c '
set -euo pipefail
# Docker gives the container a static /dev; the partition nodes write-usb.sh
# waits for appear only on devtmpfs.
[[ "$(stat -f -c %T /dev)" == devtmpfs ]] || mount -t devtmpfs devtmpfs /dev
iso_mib=$(( ($(stat -Lc%s /src/iso/sonora.iso) + 1048575) / 1048576 ))
img=/work/stick.img
truncate -s "$(( iso_mib + PERSIST_MIB ))M" "$img"
loop="$(losetup -fP --show "$img")"
trap "losetup -d $loop 2>/dev/null || true" EXIT
echo ERASE | SONORA_WRITE_USB_TEST_IMAGE="$img" bash /src/tools/write-usb.sh "$loop"
losetup -d "$loop"
trap - EXIT
sfdisk -d "$img"
cp "$img" /out/sonora-stick.img.part
'

mv -f "$ROOT/iso/sonora-stick.img.part" "$ROOT/iso/sonora-stick.img"
(cd "$ROOT/iso" && sha256sum sonora-stick.img > sonora-stick.img.sha256)
cat "$ROOT/iso/sonora-stick.img.sha256"
ls -lh "$ROOT/iso/sonora-stick.img"
echo "built: $ROOT/iso/sonora-stick.img (write it from Windows with tools/write-usb.ps1)"

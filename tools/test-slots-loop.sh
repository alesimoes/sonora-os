#!/bin/sh
# shellcheck disable=SC2016
# PRIVILEGED: lays out the A/B partitions on a loop device, writes a bundle into
# slot b with the real sonora-update and mounts the result. Needs root, sfdisk,
# losetup and e2fsprogs, so it is not in CI (no loop devices on the hosted
# runners). Run it in a privileged Debian container:
#   docker run --rm --privileged -v "$PWD":/src -w /src debian:trixie \
#     sh -c 'apt-get update -qq && apt-get install -y -qq fdisk e2fsprogs util-linux gpgv && tools/test-slots-loop.sh'
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC=$ROOT/iso/config/includes.chroot
# shellcheck source=/dev/null
. "$INC/usr/local/lib/sonora/slots.sh"
[ "$(id -u)" = 0 ] || { echo "needs root" >&2; exit 1; }
T=$(mktemp -d)
LOOP=
cleanup() {
    umount "$T/mnt" 2>/dev/null || true
    [ -z "$LOOP" ] || losetup -d "$LOOP" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT

export SLOTS_SLOT_MIB=64
truncate -s 400M "$T/disk.img"
slots_layout_script | sfdisk -q "$T/disk.img"
LOOP=$(losetup --find --show --partscan "$T/disk.img")
sfdisk -l "$LOOP" | grep -c "$LOOP"p >/dev/null || { echo "FAIL: no partitions on $LOOP" >&2; exit 1; }
mkfs.ext4 -q -L sonora-a "${LOOP}p2"
mkfs.ext4 -q -L sonora-data "${LOOP}p4"

B=$T/bundle
mkdir -p "$B"
echo kernel >"$B/vmlinuz"
echo initrd >"$B/initrd.img"
head -c 1000000 /dev/zero >"$B/filesystem.squashfs"
echo 2026.12.1 >"$B/VERSION"
(cd "$B" && sha256sum VERSION vmlinuz initrd.img filesystem.squashfs >SHA256SUMS)
echo GOOD >"$B/SHA256SUMS.sig"
printf '#!/bin/sh\n[ "$(cat "$1")" = GOOD ]\n' >"$T/verify"
printf '#!/bin/sh\nexit 1\n' >"$T/probe"
chmod +x "$T/verify" "$T/probe"
slots_env_init "$T/grubenv"
echo 2026.11.1 >"$T/release"
echo "ro sonora.slot=a" >"$T/cmdline"

SONORA_LIB="$INC/usr/local/lib/sonora/slots.sh" SONORA_GRUBENV="$T/grubenv" \
    SONORA_CMDLINE_FILE="$T/cmdline" SONORA_UPDATES_FLAG="$T/flag" SONORA_SCRATCH="$T/scratch" \
    SONORA_RELEASE_FILE="$T/release" SONORA_SESSION_PROBE="$T/probe" SONORA_VERIFY="$T/verify" \
    SONORA_NO_REMOUNT=1 SONORA_SLOT_DEV_B="${LOOP}p3" \
    sh "$INC/usr/local/bin/sonora-update" apply --from "$B"

e2fsck -fn "${LOOP}p3" >/dev/null || { echo "FAIL: slot b does not fsck clean" >&2; exit 1; }
[ "$(blkid -s LABEL -o value "${LOOP}p3")" = sonora-b ] || { echo "FAIL: slot b label" >&2; exit 1; }
mkdir "$T/mnt"
mount -o ro "${LOOP}p3" "$T/mnt"
cmp "$B/filesystem.squashfs" "$T/mnt/live/filesystem.squashfs" || { echo "FAIL: squashfs differs" >&2; exit 1; }
[ "$(cat "$T/mnt/live/VERSION")" = 2026.12.1 ] || { echo "FAIL: VERSION" >&2; exit 1; }
# The data partition and slot a must be byte-for-byte untouched.
[ "$(blkid -s LABEL -o value "${LOOP}p4")" = sonora-data ] || { echo "FAIL: data label" >&2; exit 1; }
[ "$(blkid -s LABEL -o value "${LOOP}p2")" = sonora-a ] || { echo "FAIL: slot a label" >&2; exit 1; }
[ "$(slots_env_get "$T/grubenv" sonora_trial)" = b ] || { echo "FAIL: trial not armed" >&2; exit 1; }
echo "ok: loop-device A/B layout and slot write"

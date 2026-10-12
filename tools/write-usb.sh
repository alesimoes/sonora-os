#!/usr/bin/env bash
# Write the image to a USB stick and add the persistence partition (ADR-0005).
#
# THIS ERASES THE TARGET DEVICE. Pass the whole disk, not a partition:
#   sudo tools/write-usb.sh /dev/sdb
#
# Verified by tools/test-write-usb.sh, which runs this script against a loop
# device over a disk image and boots the result. See the comment on
# SONORA_WRITE_USB_TEST_IMAGE below for the one thing that test changes.
#
# The checks before the prompt are the most important part of this file. Every
# one of them was added because the version without it would have erased a
# disk it should not have; tools/test-write-usb.sh asserts each refusal.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
DEV="${1:-}"
# The smallest persistence partition worth creating. Below this a session
# fills it in minutes, which is a worse failure than refusing now.
MIN_PERSISTENCE_MIB=256

# Test-only. When set to the path of a regular file, the target must be a loop
# device backed by exactly that file, and nothing else is accepted -- not a
# loop over some other file, and not a real disk. It replaces the removable/USB
# check, which a loop device can never pass, and relaxes nothing else.
TEST_IMAGE="${SONORA_WRITE_USB_TEST_IMAGE:-}"

die() { echo "error: $*" >&2; exit 1; }

[[ -n "$DEV" ]] || { echo "usage: $0 /dev/sdX" >&2; exit 1; }
[[ "$(id -u)" == 0 ]] || die "run as root."
[[ -f "$ISO" ]] || die "$ISO not found; run tools/build-iso.sh."
[[ -b "$DEV" ]] || die "$DEV is not a block device."

for tool in lsblk wipefs sfdisk blockdev mkfs.ext4 blkid cmp dd; do
  command -v "$tool" >/dev/null || die "$tool is required and not installed."
done

# /dev/disk/by-id/usb-... is the safest way to name a stick, and it is a
# symlink. Every check below must look at the real node.
DEV="$(readlink -f "$DEV")"
NAME="$(basename "$DEV")"

# --- what kind of device is this ---
# Ask the kernel rather than pattern-match the name. "Ends in a digit" called
# /dev/mmcblk0 and /dev/disk/by-id/usb-...-0:0 partitions, and called
# /dev/disk/by-label/persistence -- partition 3 of a SonoraOS stick -- a disk.
TYPE="$(lsblk -dno TYPE "$DEV")" || die "lsblk cannot describe $DEV."
case "$TYPE" in
  disk) ;;
  part) die "$DEV is a partition. Pass the whole disk, e.g. /dev/sdb." ;;
  loop) [[ -n "$TEST_IMAGE" ]] || die "$DEV is a loop device, not a USB stick." ;;
  *)    die "$DEV is a '$TYPE' device, not a disk." ;;
esac

if [[ -n "$TEST_IMAGE" ]]; then
  [[ "$TYPE" == loop ]] || die "SONORA_WRITE_USB_TEST_IMAGE is set, so only a loop device over that file is accepted, and $DEV is a $TYPE."
  [[ -f "$TEST_IMAGE" ]] || die "SONORA_WRITE_USB_TEST_IMAGE=$TEST_IMAGE is not a regular file."
  BACKING="$(losetup -nO BACK-FILE "$DEV" 2>/dev/null | sed 's/[[:space:]]*$//')"
  [[ -n "$BACKING" && "$(readlink -f "$BACKING")" == "$(readlink -f "$TEST_IMAGE")" ]] \
    || die "$DEV is backed by '${BACKING:-nothing}', not by $TEST_IMAGE."
  echo "TEST MODE: writing to $DEV, a loop device over $TEST_IMAGE."
else
  # A stick reports TRAN=usb; a card in a built-in reader may report mmc, or
  # only the removable flag. An internal SATA or NVMe disk reports none of
  # them, and neither does a virtual disk -- which matters, because from
  # inside a container the host's disks are visible but their mounts are not.
  RM="$(lsblk -dno RM "$DEV" | tr -d '[:space:]')"
  TRAN="$(lsblk -dno TRAN "$DEV" | tr -d '[:space:]')"
  if [[ "$TRAN" != usb && "$TRAN" != mmc && "$RM" != 1 ]]; then
    die "$DEV is not removable and not on USB (TRAN='${TRAN}', RM=${RM}). This script only writes USB sticks and cards."
  fi
fi

# --- is anything using it ---
# Refuse anything mounted, which is how people overwrite their root. Capture
# lsblk's output rather than piping it into grep: if lsblk fails, the pipe
# would read as "nothing mounted" and the check would pass on the very error
# it exists to catch.
MOUNTS="$(lsblk -nlo MOUNTPOINT "$DEV")" || die "cannot list the mounts on $DEV."
if [[ -n "$(printf '%s' "$MOUNTS" | tr -d '[:space:]')" ]]; then
  echo "error: $DEV or one of its partitions is mounted or in use as swap. Unmount it first." >&2
  lsblk -o NAME,SIZE,MODEL,MOUNTPOINT "$DEV" >&2
  exit 1
fi

# An unmounted LVM volume or an open LUKS mapping on the disk is still a
# filesystem someone is using; it shows up as a holder, not a mount.
shopt -s nullglob
for holder in /sys/class/block/"$NAME"/holders/* /sys/class/block/"$NAME"/"$NAME"*/holders/*; do
  die "$DEV is held by $(basename "$holder") (LVM, RAID or dm-crypt). Deactivate it first."
done
shopt -u nullglob

# --- will the image fit, and will its partition table mean what it says ---
SECTOR="$(blockdev --getss "$DEV")"
# The hybrid MBR inside the ISO counts in 512-byte sectors. On a 4K-sector
# device every entry would point eight times too far.
[[ "$SECTOR" == 512 ]] || die "$DEV has ${SECTOR}-byte sectors; the image's partition table assumes 512."
ISO_BYTES="$(stat -Lc%s "$ISO")"
DEV_BYTES="$(blockdev --getsize64 "$DEV")"
START_MIB=$(( (ISO_BYTES + 1048575) / 1048576 ))
NEED_MIB=$(( START_MIB + MIN_PERSISTENCE_MIB ))
(( DEV_BYTES / 1048576 >= NEED_MIB )) \
  || die "$DEV is $(( DEV_BYTES / 1048576 )) MiB; the image and the smallest useful persistence partition need ${NEED_MIB} MiB."

echo "Target device:"
lsblk -o NAME,SIZE,TYPE,RM,TRAN,VENDOR,MODEL "$DEV"
echo
read -r -p "Erase this device completely? Type ERASE to continue: " reply
[[ "$reply" == "ERASE" ]] || { echo "aborted."; exit 1; }

# --- write ---
# A stick that once carried GPT keeps a backup header in its last sectors,
# which writing over the start never touches. Some UEFI firmware then trusts
# that stale GPT over the image's MBR and finds no bootable partition.
echo "Clearing old partition-table signatures..."
wipefs --all --force "$DEV" >/dev/null

echo "Writing the image..."
dd if="$ISO" of="$DEV" bs=4M status=progress conv=fsync

# Read it back. Counterfeit and worn-out sticks accept writes they do not
# keep, and the first symptom would otherwise be a boot failure on stage.
echo "Verifying the write..."
blockdev --flushbufs "$DEV"
cmp -n "$ISO_BYTES" "$ISO" "$DEV" || die "$DEV does not read back what was written. The stick is faulty."

# --- persistence partition ---
# The image's MBR already holds two entries: 1 is the whole ISO and 2 is the
# EFI system partition *inside* it. So the new partition is number 3, and
# formatting "${DEV}2" -- as this script once would have -- destroys UEFI boot.
# parted cannot be used either: it sees xorriso's Apple partition map, reports
# the label as unknown and refuses. sfdisk edits the MBR in place.
START_SECTOR=$(( START_MIB * 1048576 / 512 ))
echo "Creating the persistence partition at ${START_MIB} MiB..."
# sfdisk's output is shown only on failure. On success it warns that the
# iso9660 signature "may remain" and recommends wipefs -- advice that, taken
# at this point, erases the image just written.
if ! sfdisk_out="$(echo "start=${START_SECTOR}, type=83" \
      | sfdisk --append --no-reread --no-tell-kernel --wipe never --wipe-partitions never "$DEV" 2>&1)"; then
  printf '%s\n' "$sfdisk_out" >&2
  die "sfdisk could not add the persistence partition to $DEV."
fi
blockdev --rereadpt "$DEV"

# Find it by where it starts, not by guessing its name: sdb3, mmcblk0p3 and
# loop0p3 are all the same partition. sysfs counts in 512-byte units.
PART=""
for _ in $(seq 1 40); do
  for dir in /sys/class/block/"$NAME"/"$NAME"*; do
    [[ -f "$dir/start" && "$(cat "$dir/start")" == "$START_SECTOR" ]] && PART="/dev/$(basename "$dir")"
  done
  [[ -n "$PART" && -b "$PART" ]] && break
  command -v udevadm >/dev/null && udevadm settle 2>/dev/null || true
  sleep 0.25
done
[[ -n "$PART" && -b "$PART" ]] || die "the kernel does not show the new partition on $DEV."

mkfs.ext4 -q -F -L persistence "$PART"

MNT="$(mktemp -d)"
trap 'umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT
mount -t ext4 "$PART" "$MNT"
# Only /home is unioned, so a damaged overlay cannot stop the system booting.
echo "/home union" > "$MNT/persistence.conf"
umount "$MNT"
sync

[[ "$(blkid -o value -s LABEL "$PART")" == persistence ]] \
  || die "$PART does not carry the persistence label after formatting."

echo
echo "Done: image on $DEV, persistence on $PART."
echo "Boot with the 'persistence' kernel parameter (the default entry already"
echo "sets it). Secure Boot must be disabled."
